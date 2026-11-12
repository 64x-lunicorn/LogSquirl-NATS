/*
 * Copyright (C) 2026 LogSquirl Contributors
 *
 * This file is part of logsquirl-nats.
 *
 * logsquirl-nats is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * logsquirl-nats is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with logsquirl-nats.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * @file permissions_test.cpp
 * @brief The NATS permissions the README lists for live capture and for
 *        replay are enough, and replay's leave the stream read-only.
 *
 * The main seam: a server with JetStream whose users have exactly the
 * README's permissions, a session that logs in as one of them, and the
 * lines of its file.
 */

// First: on Windows nats.h includes winsock2.h, which must come before any
// windows.h.
#include <nats.h>

#include <catch2/catch.hpp>

#include "capture.h"
#include "natsserver.h"
#include "natssession.h"
#include "sessiontest.h"
#include "streams.h"

#include <algorithm>

using nats_monitor::AuthMethod;
using nats_monitor::NatsSession;
using nats_monitor::SessionConfig;
using nats_test::Capture;
using nats_test::ConfiguredServer;
using nats_test::JetStream;
using nats_test::LineFields;
using nats_test::liveFields;
using nats_test::withJetStream;
using nats_test::Publisher;
using nats_test::replayConfig;
using nats_test::sessionConfig;
using nats_test::sessionTempRoot;
using nats_test::waitFor;

namespace {

/// The users: admin may do anything; capture and replay have the README's
/// permissions, for the subjects orders.> and the stream ORDERS.
const char* const kConfig = R"(
authorization {
  users = [
    { user: admin, password: admin }
    { user: capture, password: capture, permissions: {
        publish: { deny: ">" }
        subscribe: { allow: ["orders.>"] }
    } }
    { user: replay, password: replay, permissions: {
        publish: { allow: [
          "$JS.API.STREAM.NAMES",
          "$JS.API.STREAM.INFO.ORDERS",
          "$JS.API.CONSUMER.CREATE.ORDERS",
          "$JS.API.CONSUMER.INFO.ORDERS.*",
          "$JS.API.CONSUMER.DELETE.ORDERS.*",
          "$JS.FC.ORDERS.>"
        ] }
        subscribe: { allow: ["_INBOX.>"] }
    } }
  ]
}
)";

/// Log in as @p user, whose password is the same.
void logInAs( SessionConfig& config, const char* user )
{
    config.auth.method = AuthMethod::UserPassword;
    config.auth.user = QString::fromLatin1( user );
    config.auth.secret = QString::fromLatin1( user );
}

/// Whether a marker of @p capture mentions a permissions violation (nats.c
/// reports one as an asynchronous error, written as an error: marker).
bool hasViolation( const Capture& capture )
{
    const auto markers = capture.markers();
    return std::any_of( markers.begin(), markers.end(), []( const QString& text ) {
        return text.contains( QLatin1String( "violation" ), Qt::CaseInsensitive );
    } );
}

} // namespace

SCENARIO( "the permissions the README lists are enough to capture and to replay",
          "[server][permissions]" )
{
    SKIP_WITHOUT_NATS_SERVER();

    GIVEN( "a server whose users capture and replay have only the README's permissions" )
    {
        ConfiguredServer server( kConfig, withJetStream() );
        Publisher admin( server.url(), []( natsOptions* options ) {
            natsOptions_SetUserInfo( options, "admin", "admin" );
        } );
        JetStream js( admin );
        js.addStream( "ORDERS", { "orders.>" } );
        js.publishMany( "ORDERS", "orders.eu", 20000 );

        WHEN( "capture captures orders.>" )
        {
            auto config = sessionConfig( server.url(), { "orders.>" } );
            logInAs( config, "capture" );
            NatsSession session( config, sessionTempRoot() );
            QStringList errors;
            QObject::connect( &session, &NatsSession::errorOccurred,
                              [ &errors ]( const QString& message ) { errors << message; } );
            int connected = 0;
            QObject::connect( &session, &NatsSession::connected, [ &connected ] { ++connected; } );
            REQUIRE( session.start() );
            REQUIRE( waitFor( [ & ] { return connected == 1; }, 10000 ) );
            admin.publish( "orders.us", "live" );

            THEN( "the message is written, without any error" )
            {
                REQUIRE( waitFor( [ & ] {
                    return Capture::read( session.filePath() ).messages().size() == 1;
                } ) );
                session.stop();
                REQUIRE( errors.isEmpty() );
                const auto capture = Capture::read( session.filePath() );
                REQUIRE( capture.messageFields()
                         == QList<LineFields>{ liveFields( "orders.us", "live" ) } );
                REQUIRE( capture.unreadable().isEmpty() );
                REQUIRE_FALSE( capture.hasMarkerStartingWith( "error: " ) );
                REQUIRE_FALSE( hasViolation( capture ) );
            }
        }

        WHEN( "replay lists the streams and replays ORDERS" )
        {
            auto config = replayConfig( server.url(), "ORDERS" );
            logInAs( config, "replay" );
            QString error;
            const auto streams = nats_monitor::listStreams( config, &error );
            NatsSession session( config, sessionTempRoot() );
            QStringList errors;
            QObject::connect( &session, &NatsSession::errorOccurred,
                              [ &errors ]( const QString& message ) { errors << message; } );
            int completed = 0;
            QObject::connect( &session, &NatsSession::completed, [ &completed ] { ++completed; } );
            REQUIRE( session.start() );
            REQUIRE( waitFor( [ & ] { return completed == 1; }, 20000 ) );

            THEN( "ORDERS is listed, its messages are all written, and its consumer is gone" )
            {
                INFO( error.toStdString() );
                REQUIRE( streams == QStringList{ "ORDERS" } );
                const auto capture = Capture::read( session.filePath() );
                QList<quint64> all;
                for ( quint64 s = 1; s <= 20000; ++s ) {
                    all << s;
                }
                REQUIRE( capture.sequences() == all );
                REQUIRE( capture.unreadable().isEmpty() );
                REQUIRE( errors.isEmpty() );
                REQUIRE_FALSE( capture.hasMarkerStartingWith( "error: " ) );
                REQUIRE_FALSE( hasViolation( capture ) );
                REQUIRE( js.consumers( "ORDERS" ).isEmpty() );
            }
        }

        WHEN( "replay tries to change the stream" )
        {
            Publisher replay( server.url(), []( natsOptions* options ) {
                natsOptions_SetUserInfo( options, "replay", "replay" );
                natsOptions_SetTimeout( options, 1000 );
            } );
            jsCtx* context = nullptr;
            jsOptions jsOpts;
            jsOptions_Init( &jsOpts );
            jsOpts.Wait = 1000;
            Publisher::check( natsConnection_JetStream( &context, replay.connection(), &jsOpts ),
                              "natsConnection_JetStream" );
            const auto purged = js_PurgeStream( context, "ORDERS", nullptr, nullptr );
            jsPubAck* ack = nullptr;
            const auto published
                = js_Publish( &ack, context, "orders.eu", "x", 1, nullptr, nullptr );
            jsPubAck_Destroy( ack );
            jsCtx_Destroy( context );

            THEN( "it may neither purge it nor publish into it" )
            {
                REQUIRE( purged != NATS_OK );
                REQUIRE( published != NATS_OK );
                REQUIRE( js.messageCount( "ORDERS" ) == 20000 );
            }
        }
    }
}

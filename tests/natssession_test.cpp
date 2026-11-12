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
 * @file natssession_test.cpp
 * @brief BDD tests for a capture session against a real nats-server.
 *
 * The main seam of the plugin: a test starts a server, starts a session,
 * publishes with nats.c and reads the lines of the session's file.  Only
 * what the file and the session's signals show is asserted.
 */

// First: on Windows nats.h includes winsock2.h, which must come before any
// windows.h.
#include <nats.h>

#include <catch2/catch.hpp>

#include "capture.h"
#include "natsserver.h"
#include "natssession.h"
#include "profiles.h"
#include "sessiontest.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>

#include <atomic>
#include <memory>

using nats_monitor::NatsSession;
using nats_monitor::SessionConfig;
using nats_monitor::Subscription;
using nats_monitor::Transition;
using nats_test::Capture;
using nats_test::ConfiguredServer;
using nats_test::LineFields;
using nats_test::liveFields;
using nats_test::markerFields;
using nats_test::NatsServer;
using nats_test::NatsServerOptions;
using nats_test::Publisher;
using nats_test::readLines;
using nats_test::sessionConfig;
using nats_test::sessionTempRoot;
using nats_test::settle;
using nats_test::Transitions;
using nats_test::waitFor;
using Kind = nats_monitor::Transition::Kind;

namespace {

/// The text of the marker a session writes once it is subscribed to
/// @p subjects (as describeSubscriptions() names them) on @p url.
QString connectedText( const QString& url, const QString& subjects )
{
    return QStringLiteral( "connected to %1, subscribed to %2" ).arg( url, subjects );
}

/// Start @p session and wait until it is subscribed.
void startAndWaitConnected( NatsSession& session, const QString& url, const QString& subjects )
{
    REQUIRE( session.start() );
    REQUIRE( waitFor(
        [ & ] {
            return Capture::read( session.filePath() ).hasMarker( connectedText( url, subjects ) );
        },
        10000 ) );
}

} // namespace

SCENARIO( "a published message shows up as a line in the session's file", "[server][session]" )
{
    SKIP_WITHOUT_NATS_SERVER();

    GIVEN( "a session subscribed to a subject with a wildcard" )
    {
        NatsServer server;
        NatsSession session( sessionConfig( server.url(), { "orders.*.created" } ),
                             sessionTempRoot() );
        startAndWaitConnected( session, server.url(), "orders.*.created" );

        THEN( "the file is in a logsquirl-nats temporary directory" )
        {
            REQUIRE(
                QFileInfo( session.filePath() ).dir().dirName().startsWith( "logsquirl-nats-" ) );
        }

        WHEN( "a message with a reply subject and headers is published on a matching subject" )
        {
            Publisher publisher( server.url() );
            QElapsedTimer timer;
            timer.start();
            publisher.publish( "orders.eu.created", R"({"id":1,"total":9.5})", "_INBOX.ab12",
                               { { "Trace", "abc" } } );

            THEN( "it is a line of the file within a second" )
            {
                const LineFields expected{ "orders.eu.created", "_INBOX.ab12", 0, 20, "Trace=abc",
                                           R"({"id":1,"total":9.5})" };
                REQUIRE( waitFor(
                    [ & ] {
                        return Capture::read( session.filePath() ).messageFields().contains(
                            expected );
                    },
                    1000 ) );
                REQUIRE( timer.elapsed() < 1000 );
            }
        }

        WHEN( "messages are published on a matching and on another subject" )
        {
            Publisher publisher( server.url() );
            publisher.publish( "orders.eu.cancelled", "no" );
            publisher.publish( "orders.us.created", "yes" );

            THEN( "only the matching one is captured" )
            {
                REQUIRE( waitFor(
                    [ & ] {
                        return Capture::read( session.filePath() )
                            .messageFields()
                            .contains( liveFields( "orders.us.created", "yes" ) );
                    },
                    1000 ) );
                const auto capture = Capture::read( session.filePath() );
                REQUIRE( capture.messageFields()
                         == QList<LineFields>{ liveFields( "orders.us.created", "yes" ) } );
                REQUIRE( capture.unreadable().isEmpty() );
            }
        }

        WHEN( "many messages are published in a burst" )
        {
            Publisher publisher( server.url() );
            for ( int i = 0; i < 1000; ++i ) {
                publisher.publish( "orders.eu.created", QByteArray::number( i ) );
            }

            THEN( "each is one line, in order" )
            {
                REQUIRE( waitFor( [ & ] { return readLines( session.filePath() ).size() == 1001; },
                                  5000 ) );
                const auto capture = Capture::read( session.filePath() );
                for ( int i = 0; i < 1000; ++i ) {
                    REQUIRE( capture.at( i + 1 ).fields()
                             == liveFields( "orders.eu.created", QString::number( i ) ) );
                }
            }
        }
    }
}

SCENARIO( "a session subscribed to several subjects writes them all into one file",
          "[server][session][subjects]" )
{
    SKIP_WITHOUT_NATS_SERVER();

    GIVEN( "a session subscribed to orders.eu.* and billing.>" )
    {
        NatsServer server;
        NatsSession session( sessionConfig( server.url(), { "orders.eu.*", "billing.>" } ),
                             sessionTempRoot() );
        startAndWaitConnected( session, server.url(), "orders.eu.*, billing.>" );

        WHEN( "messages are published on each subject and on another" )
        {
            Publisher publisher( server.url() );
            publisher.publish( "orders.eu.created", "one" );
            publisher.publish( "billing.invoice.sent", "two" );
            publisher.publish( "orders.us.created", "none" );
            publisher.publish( "orders.eu.paid", "three" );

            THEN( "the messages of both subjects are lines of the one file, in order" )
            {
                REQUIRE( waitFor( [ & ] {
                    return Capture::read( session.filePath() )
                        .messageFields()
                        .contains( liveFields( "orders.eu.paid", "three" ) );
                } ) );
                const auto capture = Capture::read( session.filePath() );
                REQUIRE( capture.size() == 4 );
                REQUIRE( capture.at( 1 ).fields() == liveFields( "orders.eu.created", "one" ) );
                REQUIRE( capture.at( 2 ).fields() == liveFields( "billing.invoice.sent", "two" ) );
                REQUIRE( capture.at( 3 ).fields() == liveFields( "orders.eu.paid", "three" ) );
            }
        }
    }
}

SCENARIO( "overlapping subjects write a message once for each subscription",
          "[server][session][subjects]" )
{
    SKIP_WITHOUT_NATS_SERVER();

    GIVEN( "a session subscribed to orders.> and orders.eu.*" )
    {
        NatsServer server;
        NatsSession session( sessionConfig( server.url(), { "orders.>", "orders.eu.*" } ),
                             sessionTempRoot() );
        startAndWaitConnected( session, server.url(), "orders.>, orders.eu.*" );

        WHEN( "a message matching both and one matching only orders.> are published" )
        {
            Publisher publisher( server.url() );
            publisher.publish( "orders.eu.created", "both" );
            publisher.publish( "orders.us.created", "wide" );
            publisher.publish( "end", "x" ); // matches neither
            publisher.publish( "orders.eu.last", "last" );

            THEN( "the first is written twice, as the server delivered it, the second once" )
            {
                REQUIRE( waitFor( [ & ] {
                    return Capture::read( session.filePath() )
                               .messageFields()
                               .count( liveFields( "orders.eu.last", "last" ) )
                           == 2;
                } ) );
                const auto capture = Capture::read( session.filePath() );
                const auto messages = capture.messageFields();
                REQUIRE( messages.count( liveFields( "orders.eu.created", "both" ) ) == 2 );
                REQUIRE( messages.count( liveFields( "orders.us.created", "wide" ) ) == 1 );
                REQUIRE( capture.size() == 1 + 2 + 1 + 2 );
            }
        }

        WHEN( "the session is stopped" )
        {
            session.stop();

            THEN( "it ends with one stopped marker" )
            {
                const auto capture = Capture::read( session.filePath() );
                REQUIRE( capture.fields().count( markerFields( "stopped" ) ) == 1 );
                REQUIRE( capture.last().fields() == markerFields( "stopped" ) );
            }
        }
    }
}

namespace {

/// A member of a queue group outside the session, counting what it gets.
class QueueMember {
public:
    QueueMember( const QString& url, const char* subject, const char* queueGroup )
    {
        natsOptions* options = nullptr;
        natsOptions_Create( &options );
        natsOptions_SetURL( options, url.toUtf8().constData() );
        const auto status = natsConnection_Connect( &connection_, options );
        natsOptions_Destroy( options );
        REQUIRE( status == NATS_OK );
        REQUIRE( natsConnection_QueueSubscribe( &subscription_, connection_, subject, queueGroup,
                                                &QueueMember::onMessage, this )
                 == NATS_OK );
        REQUIRE( natsConnection_Flush( connection_ ) == NATS_OK );
    }

    ~QueueMember()
    {
        natsSubscription_Unsubscribe( subscription_ );
        natsSubscription_Destroy( subscription_ );
        natsConnection_Destroy( connection_ );
    }

    QueueMember( const QueueMember& ) = delete;
    QueueMember& operator=( const QueueMember& ) = delete;

    int received() const
    {
        return received_.load();
    }

private:
    static void onMessage( natsConnection*, natsSubscription*, natsMsg* msg, void* closure )
    {
        natsMsg_Destroy( msg );
        static_cast<QueueMember*>( closure )->received_.fetch_add( 1 );
    }

    natsConnection* connection_ = nullptr;
    natsSubscription* subscription_ = nullptr;
    std::atomic<int> received_{ 0 };
};

} // namespace

SCENARIO( "a subject may be subscribed in a queue group", "[server][session][subjects]" )
{
    SKIP_WITHOUT_NATS_SERVER();

    GIVEN( "a session subscribed to jobs.> in the queue group workers, and another member" )
    {
        NatsServer server;
        auto config = sessionConfig( server.url() );
        config.subscriptions = { { "jobs.>", "workers" } };
        NatsSession session( config, sessionTempRoot() );
        startAndWaitConnected( session, server.url(), "jobs.> (queue workers)" );
        QueueMember other( server.url(), "jobs.>", "workers" );

        WHEN( "200 messages are published" )
        {
            Publisher publisher( server.url() );
            for ( int i = 0; i < 200; ++i ) {
                publisher.publish( "jobs.run", QByteArray::number( i ) );
            }

            THEN( "the group shares them: each goes to the session or to the other member" )
            {
                const auto written = [ & ] {
                    return Capture::read( session.filePath() ).messagesOn( "jobs.run" ).size();
                };
                REQUIRE( waitFor( [ & ] { return written() + other.received() == 200; } ) );
                REQUIRE( written() > 0 );
                REQUIRE( other.received() > 0 );
            }
        }
    }

    GIVEN( "a session subscribed to jobs.> without a queue group, and a member of a group" )
    {
        NatsServer server;
        NatsSession session( sessionConfig( server.url(), { "jobs.>" } ), sessionTempRoot() );
        startAndWaitConnected( session, server.url(), "jobs.>" );
        QueueMember other( server.url(), "jobs.>", "workers" );

        WHEN( "20 messages are published" )
        {
            Publisher publisher( server.url() );
            for ( int i = 0; i < 20; ++i ) {
                publisher.publish( "jobs.run", QByteArray::number( i ) );
            }

            THEN( "the session takes nothing away from the group: both get every one" )
            {
                REQUIRE( waitFor( [ & ] {
                    return Capture::read( session.filePath() ).messagesOn( "jobs.run" ).size() == 20
                           && other.received() == 20;
                } ) );
            }
        }
    }
}

SCENARIO( "a session without a subject does not start", "[session][subjects]" )
{
    GIVEN( "a session with no subscription, or only a blank subject" )
    {
        const auto config = GENERATE( SessionConfig{},
                                      sessionConfig( "nats://127.0.0.1:1", { "  " } ) );
        NatsSession session( config, sessionTempRoot() );
        Transitions transitions( session );
        QStringList errors;
        QObject::connect( &session, &NatsSession::errorOccurred,
                          [ & ]( const QString& message ) { errors << message; } );

        THEN( "start() refuses: it failed, saying why as the start check does, and that alone" )
        {
            REQUIRE_FALSE( session.start() );
            REQUIRE( transitions.all()
                     == QList<Transition>{
                         { Kind::Failed, "Enter the subject to capture.", {} } } );
            REQUIRE( errors.isEmpty() );
        }
    }

    GIVEN( "a session whose config the start check refuses otherwise: a replay from "
           "sequence 0, a user and password without the user" )
    {
        auto config = sessionConfig( "nats://127.0.0.1:1", { "x" } );
        const bool replays = GENERATE( true, false );
        if ( replays ) {
            config.replay.stream = "ORDERS";
            config.replay.start = nats_monitor::ReplayStart::Sequence;
            config.replay.sequence = 0;
        }
        else {
            config.auth.method = nats_monitor::AuthMethod::UserPassword;
        }
        NatsSession session( config, sessionTempRoot() );
        Transitions transitions( session );

        THEN( "start() refuses with the start check's reason" )
        {
            REQUIRE_FALSE( session.start() );
            REQUIRE( transitions.kinds() == QList<Kind>{ Kind::Failed } );
            REQUIRE( transitions.last( Kind::Failed ).detail
                     == nats_monitor::startProblem( config ) );
            REQUIRE_FALSE( transitions.last( Kind::Failed ).detail.isEmpty() );
        }
    }
}

SCENARIO( "a session's file is named after its subjects, wildcards spelled out",
          "[session][subjects]" )
{
    GIVEN( "subjects with the wildcards > and *" )
    {
        const auto [ subscriptions, fileName ] = GENERATE( table<QList<Subscription>, QString>( {
            { { { ">", {} } }, "nats_all.log" },
            { { { "orders.>", {} } }, "nats_orders.all.log" },
            { { { "billing.*", {} } }, "nats_billing.any.log" },
            { { { "orders.*.created", {} } }, "nats_orders.any.created.log" },
            { { { "orders.>", {} }, { "billing.*", {} } }, "nats_orders.all_billing.any.log" },
            { { { "a/b", {} } }, "nats_a_b.log" },
        } ) );

        WHEN( "a session is started" )
        {
            auto config = sessionConfig( "nats://127.0.0.1:1" );
            config.subscriptions = subscriptions;
            NatsSession session( config, sessionTempRoot() );
            REQUIRE( session.start() );
            const auto name = QFileInfo( session.filePath() ).fileName();
            session.stop();

            THEN( "its file is " << fileName.toStdString() )
            {
                REQUIRE( name == fileName );
            }
        }
    }
}

SCENARIO( "stopping a session closes the connection and keeps the file", "[server][session]" )
{
    SKIP_WITHOUT_NATS_SERVER();

    GIVEN( "a running session that has captured a message" )
    {
        NatsServer server;
        Publisher publisher( server.url() );
        auto session = std::make_unique<NatsSession>( sessionConfig( server.url(), { ">" } ),
                                                      sessionTempRoot() );
        Transitions transitions( *session );
        startAndWaitConnected( *session, server.url(), ">" );
        REQUIRE( waitFor( [ & ] { return transitions.count( Kind::Running ) == 1; } ) );
        const QString path = session->filePath();
        publisher.publish( "a", "before" );
        REQUIRE( waitFor( [ & ] {
            return Capture::read( path ).messageFields().contains( liveFields( "a", "before" ) );
        } ) );

        WHEN( "it is stopped" )
        {
            session->stop();

            THEN( "it no longer runs, and the file ends with a stopped marker" )
            {
                REQUIRE_FALSE( session->isRunning() );
                REQUIRE( Capture::read( path ).last().fields() == markerFields( "stopped" ) );
            }

            THEN( "it reported each step once: connecting, running at the server, stopped" )
            {
                settle( 100 ); // nothing more comes
                REQUIRE( transitions.all()
                         == QList<Transition>{
                             { Kind::Connecting, "connecting to " + server.url() + "…", {} },
                             { Kind::Running, "connected to " + server.url(), server.url() },
                             { Kind::Stopped, "stopped", {} } } );
            }

            AND_WHEN( "it is stopped again" )
            {
                session->stop();

                THEN( "nothing more is reported" )
                {
                    REQUIRE( transitions.count( Kind::Stopped ) == 1 );
                }
            }

            AND_WHEN( "a message is published afterwards" )
            {
                publisher.publish( "a", "after" );
                settle( 300 );

                THEN( "it is not captured" )
                {
                    REQUIRE( Capture::read( path ).messageFields()
                             == QList<LineFields>{ liveFields( "a", "before" ) } );
                }
            }

            AND_WHEN( "the file is preserved and the session destroyed" )
            {
                session->preserveFile();
                session.reset();

                THEN( "the file is still there, for the tab that shows it" )
                {
                    REQUIRE( Capture::read( path ).messageFields().contains(
                        liveFields( "a", "before" ) ) );
                    QDir( QFileInfo( path ).path() ).removeRecursively();
                }
            }

            AND_WHEN( "the session is destroyed without preserving its file" )
            {
                session.reset();

                THEN( "its temporary directory is removed" )
                {
                    REQUIRE_FALSE( QFileInfo::exists( QFileInfo( path ).path() ) );
                }
            }
        }
    }
}

SCENARIO( "a session can be started and stopped repeatedly", "[server][session]" )
{
    SKIP_WITHOUT_NATS_SERVER();

    GIVEN( "a server" )
    {
        NatsServer server;
        Publisher publisher( server.url() );

        THEN( "start, stop, start, stop… each capture in its own file" )
        {
            QStringList paths;
            for ( int round = 0; round < 3; ++round ) {
                NatsSession session( sessionConfig( server.url(), { "round" } ),
                                     sessionTempRoot() );
                startAndWaitConnected( session, server.url(), "round" );
                publisher.publish( "round", QByteArray::number( round ) );
                REQUIRE( waitFor( [ & ] {
                    return Capture::read( session.filePath() )
                        .messageFields()
                        .contains( liveFields( "round", QString::number( round ) ) );
                } ) );
                session.stop();
                REQUIRE_FALSE( session.isRunning() );
                paths << QFileInfo( session.filePath() ).path();
            }
            REQUIRE( paths.removeDuplicates() == 0 );
        }
    }
}

SCENARIO( "destroying a running session stops it cleanly", "[server][session]" )
{
    SKIP_WITHOUT_NATS_SERVER();

    GIVEN( "a running session that is receiving a stream of messages" )
    {
        NatsServer server;
        Publisher publisher( server.url() );
        QString path;
        QElapsedTimer timer;
        {
            NatsSession session( sessionConfig( server.url(), { "stream" } ), sessionTempRoot() );
            startAndWaitConnected( session, server.url(), "stream" );
            session.preserveFile();
            path = session.filePath();
            for ( int i = 0; i < 2000; ++i ) {
                publisher.publish( "stream", QByteArray( 100, 'x' ), {}, {}, false );
            }
            timer.start();
        } // as on plugin unload or quit

        THEN( "it ends quickly, and the file ends with the stopped marker after whole lines" )
        {
            REQUIRE( timer.elapsed() < 5000 );
            const auto capture = Capture::read( path );
            REQUIRE( capture.last().fields() == markerFields( "stopped" ) );
            REQUIRE( capture.unreadable().isEmpty() );
            for ( const auto& line : capture.messages() ) {
                REQUIRE( line.fields() == liveFields( "stream", QString( 100, 'x' ) ) );
            }
            QDir( QFileInfo( path ).path() ).removeRecursively();
        }
    }
}

SCENARIO( "connection problems are reported and written as marker lines", "[server][session]" )
{
    SKIP_WITHOUT_NATS_SERVER();

    GIVEN( "a URL where no server listens" )
    {
        int port = 0;
        {
            NatsServer server; // a port that was free a moment ago
            port = server.port();
        }
        const QString url = QStringLiteral( "nats://127.0.0.1:%1" ).arg( port );
        NatsSession session( sessionConfig( url, { "x" } ), sessionTempRoot() );
        QStringList errors;
        QObject::connect( &session, &NatsSession::errorOccurred,
                          [ &errors ]( const QString& message ) { errors << message; } );
        Transitions transitions( session );

        WHEN( "a session is started" )
        {
            REQUIRE( session.start() );

            THEN( "it failed, saying why as in the file's marker, and the session ends" )
            {
                REQUIRE( waitFor( [ & ] { return transitions.count( Kind::Failed ) == 1; },
                                  10000 ) );
                REQUIRE( transitions.kinds() == QList<Kind>{ Kind::Connecting, Kind::Failed } );
                const auto why = transitions.last( Kind::Failed ).detail;
                REQUIRE( why.startsWith( "connection to " + url + " failed: " ) );
                REQUIRE( errors.isEmpty() ); // the transition says it, once
                REQUIRE_FALSE( session.isRunning() );
                REQUIRE( Capture::read( session.filePath() ).last().markerText() == why );
            }
        }
    }

    GIVEN( "a session connected to a server that then goes away and comes back" )
    {
        auto server = std::make_unique<NatsServer>();
        const int port = server->port();
        const QString url = server->url();
        NatsSession session( sessionConfig( url, { "back" } ), sessionTempRoot() );
        Transitions transitions( session );
        startAndWaitConnected( session, url, "back" );

        WHEN( "the server stops" )
        {
            server.reset();

            THEN( "a disconnected marker is written" )
            {
                REQUIRE( waitFor(
                    [ & ] {
                        return Capture::read( session.filePath() )
                            .fields()
                            .contains( markerFields( "disconnected" ) );
                    },
                    10000 ) );
                REQUIRE( session.isRunning() );
            }

            AND_WHEN( "a server starts on the same port again" )
            {
                NatsServerOptions options;
                options.port = port;
                server = std::make_unique<NatsServer>( options );

                THEN( "a reconnected marker is written, and messages arrive again" )
                {
                    REQUIRE( waitFor(
                        [ & ] {
                            return Capture::read( session.filePath() )
                                .hasMarker( "reconnected to " + url );
                        },
                        15000 ) );
                    Publisher publisher( url );
                    publisher.publish( "back", "again" );
                    REQUIRE( waitFor( [ & ] {
                        return Capture::read( session.filePath() )
                            .messageFields()
                            .contains( liveFields( "back", "again" ) );
                    } ) );
                    // Each step reported once: running again, as after the
                    // first connect.
                    REQUIRE( waitFor( [ & ] { return transitions.all().size() >= 4; } ) );
                    REQUIRE( transitions.all()
                             == QList<Transition>{
                                 { Kind::Connecting, "connecting to " + url + "…", {} },
                                 { Kind::Running, "connected to " + url, url },
                                 { Kind::Reconnecting, "disconnected", {} },
                                 { Kind::Running, "reconnected to " + url, url } } );
                }
            }
        }
    }
}

// ── Dropped messages ─────────────────────────────────────────────────────

namespace {

/// Publish @p count messages of 200 bytes on @p subject as fast as the
/// connection takes them, then wait until the server has them all.
void flood( Publisher& publisher, const char* subject, int count )
{
    const QByteArray payload( 200, 'x' );
    for ( int i = 0; i < count; ++i ) {
        publisher.publish( subject, payload, {}, {}, i == count - 1 );
    }
}

/// Wait until each of the @p published messages on @p subject is
/// accounted for: a line in the file, or counted as dropped.  Each message
/// is counted at most once, so the sum reaches @p published only once the
/// session has taken them all.  (Drops still to be marked are in
/// droppedCount() already; slow-consumer drops are counted when nats.c
/// delivers the next message, which it holds while it drops.)
void waitAccounted( const NatsSession& session, const char* subject, qint64 published )
{
    REQUIRE( waitFor(
        [ & ] {
            return Capture::read( session.filePath() ).messagesOn( subject ).size()
                       + static_cast<qint64>( session.droppedCount() )
                   >= published;
        },
        30000 ) );
}

} // namespace

SCENARIO( "the subscription gets the session's pending limits", "[server][session][drops]" )
{
    SKIP_WITHOUT_NATS_SERVER();

    GIVEN( "a server" )
    {
        NatsServer server;

        WHEN( "a session is started with the default configuration" )
        {
            NatsSession session( sessionConfig( server.url(), { "limits" } ), sessionTempRoot() );
            startAndWaitConnected( session, server.url(), "limits" );

            THEN( "the subscription holds up to 1,000,000 messages and 512 MiB" )
            {
                REQUIRE( session.pendingLimits().messages == 1000000 );
                REQUIRE( session.pendingLimits().bytes == 512 * 1024 * 1024 );
            }
        }

        WHEN( "a session is started with limits of its own" )
        {
            auto config = sessionConfig( server.url(), { "limits" } );
            config.pendingLimits = { 1234, 5678 * 1024 };
            NatsSession session( config, sessionTempRoot() );
            startAndWaitConnected( session, server.url(), "limits" );

            THEN( "the subscription has them" )
            {
                REQUIRE( session.pendingLimits().messages == 1234 );
                REQUIRE( session.pendingLimits().bytes == 5678 * 1024 );
            }
        }
    }
}

SCENARIO( "a slow consumer's drops are counted and marked where they happen",
          "[server][session][drops]" )
{
    SKIP_WITHOUT_NATS_SERVER();

    GIVEN( "a session whose subscription may hold a single pending message" )
    {
        NatsServer server;
        Publisher publisher( server.url() );
        auto config = sessionConfig( server.url(), { "flood" } );
        config.pendingLimits.messages = 1;
        NatsSession session( config, sessionTempRoot() );
        startAndWaitConnected( session, server.url(), "flood" );

        WHEN( "it is flooded faster than it can take the messages, and stopped" )
        {
            qint64 published = 0;
            for ( int round = 0; round < 20 && session.droppedCount() == 0; ++round ) {
                flood( publisher, "flood", 5000 );
                published += 5000;
                settle( 50 );
            }
            waitAccounted( session, "flood", published );
            session.stop();

            THEN( "the slow-consumer markers add up to the messages not written" )
            {
                const auto capture = Capture::read( session.filePath() );
                const auto drops = capture.drops();
                const qint64 written = capture.messagesOn( "flood" ).size();
                REQUIRE( drops.unknown == 0 );
                REQUIRE( drops.slowConsumer > 0 );
                REQUIRE( drops.queueFull == 0 );
                REQUIRE( drops.total() == published - written );
                REQUIRE( session.droppedCount() == static_cast<quint64>( drops.total() ) );
                REQUIRE( capture.last().fields() == markerFields( "stopped" ) );
            }
        }
    }
}

SCENARIO( "the drops of all subscriptions of a session add up in its markers",
          "[server][session][drops][subjects]" )
{
    SKIP_WITHOUT_NATS_SERVER();

    GIVEN( "a session of two subscriptions that may each hold a single pending message" )
    {
        NatsServer server;
        Publisher publisher( server.url() );
        auto config = sessionConfig( server.url(), { "a.flood", "b.flood" } );
        config.pendingLimits.messages = 1;
        NatsSession session( config, sessionTempRoot() );
        startAndWaitConnected( session, server.url(), "a.flood, b.flood" );

        WHEN( "both are flooded faster than it can take the messages, and it is stopped" )
        {
            qint64 published = 0;
            for ( int round = 0; round < 20 && session.droppedCount() == 0; ++round ) {
                flood( publisher, "a.flood", 5000 );
                flood( publisher, "b.flood", 5000 );
                published += 10000;
                settle( 50 );
            }
            REQUIRE( waitFor(
                [ & ] {
                    const auto capture = Capture::read( session.filePath() );
                    return capture.messagesOn( "a.flood" ).size()
                               + capture.messagesOn( "b.flood" ).size()
                               + static_cast<qint64>( session.droppedCount() )
                           >= published;
                },
                30000 ) );
            session.stop();

            THEN( "the session's slow-consumer markers add up to the messages of both not "
                  "written" )
            {
                const auto capture = Capture::read( session.filePath() );
                const auto drops = capture.drops();
                const qint64 a = capture.messagesOn( "a.flood" ).size();
                const qint64 b = capture.messagesOn( "b.flood" ).size();
                REQUIRE( drops.unknown == 0 );
                REQUIRE( drops.slowConsumer > 0 );
                REQUIRE( drops.total() == published - a - b );
                REQUIRE( session.droppedCount() == static_cast<quint64>( drops.total() ) );
                REQUIRE( capture.last().fields() == markerFields( "stopped" ) );
            }
        }
    }
}

SCENARIO( "under a flood every drop is counted in exactly one marker", "[server][session][drops]" )
{
    SKIP_WITHOUT_NATS_SERVER();

    GIVEN( "a session with small pending limits and a small queue" )
    {
        NatsServer server;
        Publisher publisher( server.url() );
        auto config = sessionConfig( server.url(), { "flood" } );
        config.pendingLimits.messages = 16;
        config.queueLimit = 1;
        NatsSession session( config, sessionTempRoot() );
        startAndWaitConnected( session, server.url(), "flood" );
        const qint64 published = 50000;

        WHEN( "it is flooded faster than it can write, and stopped once it has taken it all" )
        {
            flood( publisher, "flood", published );
            waitAccounted( session, "flood", published );
            session.stop();

            THEN( "the dropped= counts of both kinds add up to published - written, and to the "
                  "session's dropped total" )
            {
                const auto capture = Capture::read( session.filePath() );
                const auto drops = capture.drops();
                const qint64 written = capture.messagesOn( "flood" ).size();
                REQUIRE( drops.unknown == 0 );
                REQUIRE( drops.slowConsumer > 0 );
                REQUIRE( drops.queueFull > 0 );
                REQUIRE( drops.total() == published - written );
                REQUIRE( session.droppedCount() == static_cast<quint64>( drops.total() ) );
            }
        }

        WHEN( "it is stopped right after the flood, still dropping" )
        {
            flood( publisher, "flood", published );
            session.stop();

            THEN( "every message published is written or counted in a marker before the stopped "
                  "marker, none lost" )
            {
                const auto capture = Capture::read( session.filePath() );
                const auto drops = capture.drops();
                const qint64 written = capture.messagesOn( "flood" ).size();
                REQUIRE( drops.unknown == 0 );
                REQUIRE( drops.total() > 0 );
                REQUIRE( drops.total() == published - written );
                REQUIRE( session.droppedCount() == static_cast<quint64>( drops.total() ) );
                // What reached the session was written or dropped from its queue.
                REQUIRE( session.receivedCount()
                         == static_cast<quint64>( written + drops.queueFull ) );
                REQUIRE( capture.last().fields() == markerFields( "stopped" ) );
            }
        }
    }
}

SCENARIO( "when the server goes away during a flood, the drops until then are still marked",
          "[server][session][drops]" )
{
    SKIP_WITHOUT_NATS_SERVER();

    GIVEN( "a session with small pending limits, flooded, whose server then goes away" )
    {
        auto server = std::make_unique<NatsServer>();
        const int port = server->port();
        const QString url = server->url();
        auto config = sessionConfig( url, { "flood" } );
        config.pendingLimits.messages = 16;
        config.queueLimit = 1;
        NatsSession session( config, sessionTempRoot() );
        Transitions transitions( session );
        startAndWaitConnected( session, url, "flood" );
        {
            Publisher publisher( url );
            flood( publisher, "flood", 50000 );
        }
        server.reset(); // while the session is still taking the flood

        // Messages the server had not sent yet are gone with it: what is
        // checked is that each message nats.c took is written or counted.
        const auto check = [ & ]( const QString& lastMarker ) {
            const auto capture = Capture::read( session.filePath() );
            const auto drops = capture.drops();
            const qint64 written = capture.messagesOn( "flood" ).size();
            REQUIRE( drops.unknown == 0 );
            REQUIRE( drops.slowConsumer > 0 );
            REQUIRE( session.droppedCount() == static_cast<quint64>( drops.total() ) );
            REQUIRE( session.receivedCount()
                     == static_cast<quint64>( written + drops.queueFull ) );
            REQUIRE( capture.last().fields() == markerFields( lastMarker ) );
        };

        WHEN( "the session is stopped while disconnected" )
        {
            REQUIRE( waitFor(
                [ & ] { return Capture::read( session.filePath() ).hasMarker( "disconnected" ); },
                10000 ) );
            QElapsedTimer timer;
            timer.start();
            session.stop();

            THEN( "it stops at once, and every drop is in a marker before the stopped marker" )
            {
                REQUIRE( timer.elapsed() < 3000 );
                check( "stopped" );
            }
        }

        WHEN( "the connection is then closed for good (a server on the port refuses it)" )
        {
            NatsServerOptions options;
            options.port = port;
            ConfiguredServer refusing( "authorization { token: \"not-the-session's\" }\n",
                                       options );

            THEN( "the session ends, and every drop is in a marker before the closed marker" )
            {
                REQUIRE( waitFor( [ & ] { return transitions.count( Kind::Failed ) == 1; },
                                  30000 ) );
                REQUIRE( transitions.last( Kind::Failed ).detail == "connection closed" );
                check( "connection closed" );
            }
        }
    }
}

// Opt-in, not run by ctest or CI (hidden tag; no [server], which ctest
// selects): logsquirl_nats_tests "[benchmark]".  See DEVELOPER_GUIDE.md.
SCENARIO( "a session sustains 50k msgs/s at 200 B without drops", "[.benchmark]" )
{
    SKIP_WITHOUT_NATS_SERVER();

    constexpr int kRate = 50000;  // messages per second
    constexpr int kSeconds = 10;  // how long it is published
    constexpr int kPayload = 200; // bytes
    constexpr qint64 kTotal = qint64( kRate ) * kSeconds;

    GIVEN( "a session with the default limits" )
    {
        NatsServer server;
        Publisher publisher( server.url() );
        NatsSession session( sessionConfig( server.url(), { "bench" } ), sessionTempRoot() );
        startAndWaitConnected( session, server.url(), "bench" );

        WHEN( "50k msgs/s of 200 B are published for 10 seconds" )
        {
            const QByteArray payload( kPayload, 'x' );
            QElapsedTimer timer;
            timer.start();
            qint64 sent = 0;
            while ( sent < kTotal ) {
                const qint64 due = std::min( kTotal, timer.nsecsElapsed() * kRate / 1000000000 );
                for ( ; sent < due; ++sent ) {
                    publisher.publish( "bench", payload, {}, {}, false );
                }
                QThread::usleep( 200 );
            }
            publisher.publish( "bench", payload, {}, {}, true );
            ++sent;
            const double publishSeconds = timer.nsecsElapsed() / 1e9;
            REQUIRE(
                waitFor( [ & ] { return session.receivedCount() >= quint64( sent ); }, 30000 ) );
            const double takenSeconds = timer.nsecsElapsed() / 1e9;
            waitAccounted( session, "bench", sent );
            session.stop();

            THEN( "every message is written, nothing is dropped, and no dropped= marker" )
            {
                const auto capture = Capture::read( session.filePath() );
                const auto drops = capture.drops();
                const qint64 written = capture.messagesOn( "bench" ).size();
                WARN( "published " << sent << " messages of " << kPayload << " B in "
                                   << publishSeconds << " s (" << qRound( sent / publishSeconds )
                                   << " msgs/s); received in " << takenSeconds << " s; written "
                                   << written << ", dropped " << session.droppedCount() );
                REQUIRE( drops.markers == 0 );
                REQUIRE( drops.unknown == 0 );
                REQUIRE( session.droppedCount() == 0 );
                REQUIRE( written == sent );
            }
        }
    }
}

SCENARIO( "without drops no dropped marker is written, and the session counts what it received",
          "[server][session][drops]" )
{
    SKIP_WITHOUT_NATS_SERVER();

    GIVEN( "a session with the default limits" )
    {
        NatsServer server;
        Publisher publisher( server.url() );
        NatsSession session( sessionConfig( server.url(), { "calm" } ), sessionTempRoot() );
        startAndWaitConnected( session, server.url(), "calm" );

        WHEN( "a burst it can keep up with is published, and the session stopped" )
        {
            for ( int i = 0; i < 1000; ++i ) {
                publisher.publish( "calm", QByteArray( 200, 'x' ), {}, {}, i % 100 == 99 );
            }
            REQUIRE(
                waitFor( [ & ] { return readLines( session.filePath() ).size() == 1001; }, 5000 ) );
            session.stop();

            THEN( "every message is a line, no line says dropped=, and the counts say so" )
            {
                const auto capture = Capture::read( session.filePath() );
                REQUIRE( capture.size() == 1002 ); // connected, 1000 messages, stopped
                REQUIRE( capture.messagesOn( "calm" ).size() == 1000 );
                REQUIRE( capture.drops().markers == 0 );
                REQUIRE( capture.drops().unknown == 0 );
                REQUIRE( session.receivedCount() == 1000 );
                REQUIRE( session.droppedCount() == 0 );
            }
        }
    }
}

// ── Rotation at the size limit ──────────────────────────────────────────

namespace {

/// The files of the capture whose first file is @p first, in order:
/// nats_<name>.log, nats_<name>.2.log, nats_<name>.3.log, …
QStringList captureFiles( const QString& first )
{
    QStringList files;
    if ( !QFileInfo( first ).isFile() ) {
        return files;
    }
    files << first;
    auto stem = first;
    stem.chop( 4 ); // ".log"
    for ( int n = 2;; ++n ) {
        const auto next = QStringLiteral( "%1.%2.log" ).arg( stem ).arg( n );
        if ( !QFileInfo( next ).isFile() ) {
            return files;
        }
        files << next;
    }
}

qint64 sizeOf( const QString& path )
{
    return QFileInfo( path ).size();
}

/// Whether the file at @p path is empty or ends with a line terminator.
bool endsWithNewline( const QString& path )
{
    QFile file( path );
    if ( !file.open( QIODevice::ReadOnly ) ) {
        return false;
    }
    const auto content = file.readAll();
    return content.isEmpty() || content.endsWith( '\n' );
}

/// "m0001", "m0002", …, padded with x to @p size bytes.
QByteArray numbered( int i, int size )
{
    return QStringLiteral( "m%1" )
        .arg( i, 4, 10, QLatin1Char( '0' ) )
        .toLatin1()
        .leftJustified( size, 'x' );
}

} // namespace

SCENARIO( "a capture's file is rotated at its size limit, between messages",
          "[server][session][rotation]" )
{
    SKIP_WITHOUT_NATS_SERVER();

    GIVEN( "a session with a size limit of 1000 bytes" )
    {
        NatsServer server;
        Publisher publisher( server.url() );
        auto config = sessionConfig( server.url(), { "rot" } );
        config.maxFileBytes = 1000;
        NatsSession session( config, sessionTempRoot() );
        QStringList rotatedTo;
        QObject::connect( &session, &NatsSession::fileRotated,
                          [ & ]( const QString& path ) { rotatedTo << path; } );
        startAndWaitConnected( session, server.url(), "rot" );
        const auto first = session.filePath();

        WHEN( "messages of about 100 bytes a line, 3500 bytes in all, are published" )
        {
            QStringList published;
            for ( int i = 1; i <= 35; ++i ) {
                const auto payload = numbered( i, 60 );
                published << QString::fromLatin1( payload );
                publisher.publish( "rot", payload, {}, {}, i == 35 );
            }
            REQUIRE( waitFor( [ & ] {
                return Capture::read( captureFiles( first ) ).payloadsOn( "rot" ).size() == 35;
            } ) );
            session.stop();
            const auto files = captureFiles( first );

            THEN( "the capture goes on in nats_rot.2.log, nats_rot.3.log, …, each announced" )
            {
                REQUIRE( files.size() == 4 );
                REQUIRE( QFileInfo( files.at( 1 ) ).fileName() == "nats_rot.2.log" );
                REQUIRE( QFileInfo( files.at( 3 ) ).fileName() == "nats_rot.4.log" );
                REQUIRE( waitFor( [ & ] { return rotatedTo.size() == 3; } ) );
                REQUIRE( rotatedTo == files.mid( 1 ) );
                REQUIRE( session.filePath() == files.last() );
            }

            THEN( "no file is over the limit, and each was rotated only when the next line "
                  "would not have fit" )
            {
                for ( int i = 0; i < files.size(); ++i ) {
                    INFO( files.at( i ).toStdString() );
                    REQUIRE( sizeOf( files.at( i ) ) <= 1000 );
                    REQUIRE( endsWithNewline( files.at( i ) ) );
                    if ( i + 1 < files.size() ) {
                        const auto next = readLines( files.at( i + 1 ) );
                        REQUIRE_FALSE( next.isEmpty() );
                        const auto nextLine = next.first().toUtf8().size() + 1;
                        REQUIRE( sizeOf( files.at( i ) ) + nextLine > 1000 );
                    }
                }
            }

            THEN( "every message is one whole line, in order across the files; the first file "
                  "starts connected, the last ends stopped" )
            {
                REQUIRE( Capture::read( files ).payloadsOn( "rot" ) == published );
                REQUIRE( Capture::read( files.first() ).first().markerText()
                         == connectedText( server.url(), "rot" ) );
                REQUIRE( Capture::read( files.last() ).last().fields()
                         == markerFields( "stopped" ) );
            }
        }

        WHEN( "a message longer than the limit is published" )
        {
            publisher.publish( "rot", QByteArray( 1500, 'L' ) );
            publisher.publish( "rot", "after" );
            REQUIRE( waitFor( [ & ] {
                return Capture::read( captureFiles( first ) ).payloadsOn( "rot" ).size() == 2;
            } ) );
            session.stop();
            const auto files = captureFiles( first );

            THEN( "it is written whole, alone in a file of its own" )
            {
                REQUIRE( files.size() == 3 );
                REQUIRE( Capture::read( files.at( 1 ) ).fields()
                         == QList<LineFields>{ liveFields( "rot", QString( 1500, 'L' ) ) } );
                REQUIRE( Capture::read( files.at( 2 ) ).payloadsOn( "rot" )
                         == QStringList{ "after" } );
            }
        }
    }
}

SCENARIO( "a session started again goes on in its last file, within the size limit",
          "[server][session][rotation]" )
{
    SKIP_WITHOUT_NATS_SERVER();

    GIVEN( "a session with a size limit of 1000 bytes that rotated once and was stopped" )
    {
        NatsServer server;
        Publisher publisher( server.url() );
        auto config = sessionConfig( server.url(), { "again" } );
        config.maxFileBytes = 1000;
        NatsSession session( config, sessionTempRoot() );
        Transitions transitions( session );
        startAndWaitConnected( session, server.url(), "again" );
        const auto first = session.filePath();

        int published = 0;
        const auto publish = [ & ]( int count ) {
            for ( int i = 0; i < count; ++i ) {
                ++published;
                publisher.publish( "again", numbered( published, 60 ), {}, {}, i == count - 1 );
            }
            REQUIRE( waitFor( [ & ] {
                return Capture::read( captureFiles( first ) ).payloadsOn( "again" ).size()
                       == published;
            } ) );
        };
        publish( 15 ); // about 1500 bytes
        session.stop();
        REQUIRE( captureFiles( first ).size() == 2 );
        const auto last = captureFiles( first ).last();
        REQUIRE( sizeOf( last ) > 0 );

        WHEN( "it is started again and gets another 1500 bytes" )
        {
            REQUIRE( session.start() );
            REQUIRE( waitFor( [ & ] { return transitions.count( Kind::Running ) == 2; }, 10000 ) );
            REQUIRE( session.filePath() == last );
            publish( 15 );
            session.stop();
            const auto files = captureFiles( first );

            THEN( "no file is over the limit, and every message is there, in order" )
            {
                REQUIRE( files.size() >= 3 );
                for ( const auto& file : files ) {
                    INFO( file.toStdString() );
                    REQUIRE( sizeOf( file ) <= 1000 );
                    REQUIRE( endsWithNewline( file ) );
                }
                QStringList expected;
                for ( int i = 1; i <= 30; ++i ) {
                    expected << QString::fromLatin1( numbered( i, 60 ) );
                }
                REQUIRE( Capture::read( files ).payloadsOn( "again" ) == expected );
            }
        }
    }
}

SCENARIO( "a rotation that fails is retried one limit later", "[server][session][rotation]" )
{
    SKIP_WITHOUT_NATS_SERVER();

    GIVEN( "a session with a size limit of 1000 bytes whose next file cannot be created" )
    {
        NatsServer server;
        Publisher publisher( server.url() );
        auto config = sessionConfig( server.url(), { "rf" } );
        config.maxFileBytes = 1000;
        NatsSession session( config, sessionTempRoot() );
        QStringList errors;
        QStringList rotatedTo;
        QObject::connect( &session, &NatsSession::errorOccurred,
                          [ & ]( const QString& message ) { errors << message; } );
        QObject::connect( &session, &NatsSession::fileRotated,
                          [ & ]( const QString& path ) { rotatedTo << path; } );
        startAndWaitConnected( session, server.url(), "rf" );
        const auto first = session.filePath();
        // A directory where the next file goes: opening it fails, on every
        // platform and also for root.
        const auto second = QFileInfo( first ).dir().filePath( "nats_rf.2.log" );
        REQUIRE( QDir().mkdir( second ) );

        int published = 0;
        const auto publish = [ & ]( int count ) {
            for ( int i = 0; i < count; ++i ) {
                ++published;
                publisher.publish( "rf", numbered( published, 60 ), {}, {}, i == count - 1 );
            }
            REQUIRE( waitFor( [ & ] {
                return Capture::read( captureFiles( first ) ).payloadsOn( "rf" ).size()
                       == published;
            } ) );
            settle( 50 ); // for the signals
        };

        WHEN( "the file reaches the limit" )
        {
            publish( 12 ); // about 1300 bytes

            THEN( "the failure is reported, and the capture goes on in the same file" )
            {
                REQUIRE( errors.size() == 1 );
                REQUIRE( errors.first().contains( "nats_rf.2.log" ) );
                REQUIRE( rotatedTo.isEmpty() );
                REQUIRE( captureFiles( first ) == QStringList{ first } );
                REQUIRE( sizeOf( first ) > 1000 );
                REQUIRE( Capture::read( first ).payloadsOn( "rf" ).size() == 12 );
            }

            AND_WHEN( "the next file could be created now, and the file grows by less than "
                      "the limit since the failure" )
            {
                REQUIRE( QDir().rmdir( second ) );
                publish( 4 ); // about 1700 bytes

                THEN( "it is not retried on every line" )
                {
                    REQUIRE( errors.size() == 1 );
                    REQUIRE_FALSE( QFileInfo::exists( second ) );
                    REQUIRE( session.filePath() == first );
                }

                AND_WHEN( "it has grown by the limit since the failure" )
                {
                    publish( 10 ); // about 2700 bytes

                    THEN( "the capture goes on in nats_rf.2.log, and nothing is lost" )
                    {
                        REQUIRE( errors.size() == 1 );
                        REQUIRE( captureFiles( first ) == QStringList{ first, second } );
                        REQUIRE( rotatedTo == QStringList{ second } );
                        REQUIRE( sizeOf( first ) <= 2000 );
                        const auto payloads
                            = Capture::read( QStringList{ first, second } ).payloadsOn( "rf" );
                        REQUIRE( payloads.size() == 26 );
                        REQUIRE( payloads.last() == QString::fromLatin1( numbered( 26, 60 ) ) );
                    }
                }
            }
        }
    }
}

SCENARIO( "a size limit of 0 means no limit", "[server][session][rotation]" )
{
    SKIP_WITHOUT_NATS_SERVER();

    GIVEN( "a session with a size limit of 0" )
    {
        NatsServer server;
        Publisher publisher( server.url() );
        auto config = sessionConfig( server.url(), { "nolimit" } );
        config.maxFileBytes = 0;
        NatsSession session( config, sessionTempRoot() );
        startAndWaitConnected( session, server.url(), "nolimit" );
        const auto first = session.filePath();

        WHEN( "100 KB are published" )
        {
            for ( int i = 1; i <= 100; ++i ) {
                publisher.publish( "nolimit", numbered( i, 1000 ), {}, {}, i == 100 );
            }
            REQUIRE( waitFor(
                [ & ] { return Capture::read( first ).payloadsOn( "nolimit" ).size() == 100; } ) );
            session.stop();

            THEN( "they all stay in the one file" )
            {
                REQUIRE( captureFiles( first ) == QStringList{ first } );
                REQUIRE( sizeOf( first ) > 100000 );
                REQUIRE( session.filePath() == first );
            }
        }
    }
}

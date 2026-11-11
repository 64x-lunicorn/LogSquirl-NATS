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

#include "natsserver.h"
#include "natssession.h"
#include "sessiontest.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QRegularExpression>

#include <memory>

using nats_monitor::NatsSession;
using nats_monitor::SessionConfig;
using nats_test::NatsServer;
using nats_test::NatsServerOptions;
using nats_test::Publisher;
using nats_test::readLines;
using nats_test::settle;
using nats_test::waitFor;

namespace {

SessionConfig configFor( const QString& url, const QString& subject )
{
    SessionConfig config;
    config.url = url;
    config.subject = subject;
    return config;
}

/// The marker line a session writes once it is subscribed.
QString connectedMarker( const QString& url, const QString& subject )
{
    return QStringLiteral( " \\$NATS-MONITOR - \\d+" )
           + QRegularExpression::escape(
               QStringLiteral( " connected to %1, subscribed to %2" ).arg( url, subject ) )
           + "$";
}

/// Whether some line of @p lines matches @p pattern.
bool anyMatches( const QStringList& lines, const QString& pattern )
{
    const QRegularExpression re( pattern );
    for ( const auto& line : lines ) {
        if ( re.match( line ).hasMatch() ) {
            return true;
        }
    }
    return false;
}

/// Start @p session and wait until it is subscribed.
void startAndWaitConnected( NatsSession& session, const QString& url, const QString& subject )
{
    REQUIRE( session.start() );
    REQUIRE( waitFor(
        [ & ] {
            return anyMatches( readLines( session.filePath() ), connectedMarker( url, subject ) );
        },
        10000 ) );
}

const QString kTimestamp
    = QStringLiteral( "^\\d{4}-\\d\\d-\\d\\d \\d\\d:\\d\\d:\\d\\d\\.\\d{6}Z " );

} // namespace

SCENARIO( "a published message shows up as a line in the session's file", "[server][session]" )
{
    SKIP_WITHOUT_NATS_SERVER();

    GIVEN( "a session subscribed to a subject with a wildcard" )
    {
        NatsServer server;
        NatsSession session( configFor( server.url(), "orders.*.created" ) );
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
                const QString expected
                    = kTimestamp
                      + QRegularExpression::escape(
                          R"(orders.eu.created _INBOX.ab12 20 [Trace=abc] {"id":1,"total":9.5})" )
                      + "$";
                REQUIRE( waitFor(
                    [ & ] { return anyMatches( readLines( session.filePath() ), expected ); },
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
                        return anyMatches( readLines( session.filePath() ),
                                           kTimestamp + "orders\\.us\\.created - 3 yes$" );
                    },
                    1000 ) );
                REQUIRE_FALSE(
                    anyMatches( readLines( session.filePath() ), "orders\\.eu\\.cancelled" ) );
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
                const auto lines = readLines( session.filePath() );
                for ( int i = 0; i < 1000; ++i ) {
                    REQUIRE( lines.at( i + 1 ).endsWith( QStringLiteral( " %1" ).arg( i ) ) );
                }
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
        auto session = std::make_unique<NatsSession>( configFor( server.url(), ">" ) );
        startAndWaitConnected( *session, server.url(), ">" );
        const QString path = session->filePath();
        publisher.publish( "a", "before" );
        REQUIRE( waitFor( [ & ] { return anyMatches( readLines( path ), " a - 6 before$" ); } ) );

        WHEN( "it is stopped" )
        {
            session->stop();

            THEN( "it no longer runs, and the file ends with a stopped marker" )
            {
                REQUIRE_FALSE( session->isRunning() );
                REQUIRE( readLines( path ).last().endsWith( " $NATS-MONITOR - 7 stopped" ) );
            }

            AND_WHEN( "a message is published afterwards" )
            {
                publisher.publish( "a", "after" );
                settle( 300 );

                THEN( "it is not captured" )
                {
                    REQUIRE_FALSE( anyMatches( readLines( path ), "after" ) );
                }
            }

            AND_WHEN( "the file is preserved and the session destroyed" )
            {
                session->preserveFile();
                session.reset();

                THEN( "the file is still there, for the tab that shows it" )
                {
                    REQUIRE( anyMatches( readLines( path ), " a - 6 before$" ) );
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
                NatsSession session( configFor( server.url(), "round" ) );
                startAndWaitConnected( session, server.url(), "round" );
                publisher.publish( "round", QByteArray::number( round ) );
                REQUIRE( waitFor( [ & ] {
                    return anyMatches( readLines( session.filePath() ),
                                       QStringLiteral( " round - 1 %1$" ).arg( round ) );
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
            NatsSession session( configFor( server.url(), "stream" ) );
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
            const auto lines = readLines( path );
            REQUIRE( lines.last().endsWith( " $NATS-MONITOR - 7 stopped" ) );
            for ( const auto& line : lines ) {
                REQUIRE( QRegularExpression( kTimestamp ).match( line ).hasMatch() );
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
        NatsSession session( configFor( url, "x" ) );
        QStringList errors;
        QObject::connect( &session, &NatsSession::errorOccurred,
                          [ &errors ]( const QString& message ) { errors << message; } );
        int finished = 0;
        QObject::connect( &session, &NatsSession::finished, [ &finished ] { ++finished; } );

        WHEN( "a session is started" )
        {
            REQUIRE( session.start() );

            THEN( "the failure is reported, written to the file, and the session ends" )
            {
                REQUIRE( waitFor( [ & ] { return finished == 1; }, 10000 ) );
                REQUIRE( errors.size() == 1 );
                REQUIRE( errors.first().contains( url ) );
                REQUIRE_FALSE( session.isRunning() );
                REQUIRE( anyMatches( readLines( session.filePath() ),
                                     kTimestamp + "\\$NATS-MONITOR - \\d+ connection to "
                                         + QRegularExpression::escape( url ) + " failed: " ) );
            }
        }
    }

    GIVEN( "a session connected to a server that then goes away and comes back" )
    {
        auto server = std::make_unique<NatsServer>();
        const int port = server->port();
        const QString url = server->url();
        NatsSession session( configFor( url, "back" ) );
        QStringList status;
        QObject::connect( &session, &NatsSession::statusChanged,
                          [ &status ]( const QString& text ) { status << text; } );
        QStringList connected;
        QObject::connect( &session, &NatsSession::connected,
                          [ &connected ]( const QString& server ) { connected << server; } );
        startAndWaitConnected( session, url, "back" );

        WHEN( "the server stops" )
        {
            server.reset();

            THEN( "a disconnected marker is written" )
            {
                REQUIRE( waitFor(
                    [ & ] {
                        return anyMatches( readLines( session.filePath() ),
                                           kTimestamp + "\\$NATS-MONITOR - 12 disconnected$" );
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
                            return anyMatches( readLines( session.filePath() ),
                                               kTimestamp + "\\$NATS-MONITOR - \\d+ reconnected to "
                                                   + QRegularExpression::escape( url ) + "$" );
                        },
                        15000 ) );
                    Publisher publisher( url );
                    publisher.publish( "back", "again" );
                    REQUIRE( waitFor( [ & ] {
                        return anyMatches( readLines( session.filePath() ), " back - 5 again$" );
                    } ) );
                    // The dialog shows them too.
                    REQUIRE( waitFor( [ & ] { return status.size() >= 3; } ) );
                    REQUIRE( status.at( 0 ) == "connected to " + url );
                    REQUIRE( status.at( 1 ) == "disconnected" );
                    REQUIRE( status.at( 2 ) == "reconnected to " + url );
                    // Connected again, as after the first connect.
                    REQUIRE( connected == QStringList{ url, url } );
                }
            }
        }
    }
}

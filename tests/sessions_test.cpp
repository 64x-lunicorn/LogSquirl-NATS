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
 * @file sessions_test.cpp
 * @brief BDD tests for Sessions, the one owner of each connection's status
 *        (issue #29), which reaches the host through a host adapter
 *        (issue #30).
 *
 * Sessions is driven without the plugin: an in-memory host (memoryhost.h)
 * records the tabs it opens, its notifications and its log, and a clock of
 * the test's makes the rate exact.  The status is read with status(), and
 * what changed() announced is recorded.
 */

// First: on Windows nats.h includes winsock2.h, which must come before any
// windows.h.
#include <nats.h>

#include <catch2/catch.hpp>

#include "memoryhost.h"
#include "natsserver.h"
#include "profiles.h"
#include "sessions.h"
#include "sessiontest.h"

#include <QDir>
#include <QTemporaryDir>

#include <memory>
#include <utility>

using nats_monitor::ConnectionProfile;
using nats_monitor::Sessions;
using nats_monitor::SessionState;
using nats_test::MemoryHost;
using nats_test::NatsServer;
using nats_test::Publisher;
using nats_test::sessionTempRoot;
using nats_test::waitFor;

namespace {

/// The connection @p name on @p url capturing @p subjects.
ConnectionProfile profile( const QString& name, const QString& url, const QString& subjects )
{
    ConnectionProfile profile;
    profile.name = name;
    profile.url = url;
    profile.subjects = subjects;
    return profile;
}

/// The states and details Sessions announced for a connection, each change
/// of either once: what its transitions were.
struct Announced {
    Announced( Sessions& sessions, const QString& name )
    {
        QObject::connect( &sessions, &Sessions::changed,
                          [ &sessions, name, list = list ]( const QString& changed ) {
                              if ( changed != name ) {
                                  return;
                              }
                              const auto status = sessions.status( name );
                              const std::pair<SessionState, QString> step{ status.state,
                                                                           status.detail };
                              if ( list->isEmpty() || list->last() != step ) {
                                  list->append( step );
                              }
                          } );
    }

    QList<SessionState> states() const
    {
        QList<SessionState> states;
        for ( const auto& step : *list ) {
            states << step.first;
        }
        return states;
    }

    std::shared_ptr<QList<std::pair<SessionState, QString>>> list
        = std::make_shared<QList<std::pair<SessionState, QString>>>();
};

} // namespace

SCENARIO( "Sessions keeps the state, counts and rate of each connection", "[server][sessions]" )
{
    SKIP_WITHOUT_NATS_SERVER();

    GIVEN( "Sessions on an in-memory host and a clock of the test's, and a server" )
    {
        NatsServer server;
        MemoryHost host;
        auto nowMs = std::make_shared<qint64>( 10000 );
        Sessions sessions( host, sessionTempRoot(), nullptr, [ nowMs ] { return *nowMs; } );
        Announced announced( sessions, "orders" );

        WHEN( "the connection orders is started" )
        {
            auto orders = profile( "orders", server.url(), "orders.>" );
            REQUIRE( sessions.start( orders ) );

            THEN( "it connects, saying where to, and writes its file" )
            {
                const auto status = sessions.status( "orders" );
                REQUIRE( status.state == SessionState::Connecting );
                REQUIRE( status.detail == "connecting to " + server.url() + "…" );
                REQUIRE_FALSE( status.filePath.isEmpty() );
                REQUIRE( status.files == 1 );
                REQUIRE( sessions.isRunning( "orders" ) );
            }

            THEN( "its file is opened in one followed tab, and a notification says it started" )
            {
                REQUIRE( host.followedTabs == QStringList{ sessions.status( "orders" ).filePath } );
                REQUIRE(
                    host.notifications
                    == QStringList{ "NATS capture of orders.> started on " + server.url() + "." } );
            }

            AND_WHEN( "it is stopped with a notification" )
            {
                sessions.stop( "orders" );

                THEN( "it returns with the connection stopping, which cannot be started again" )
                {
                    REQUIRE( sessions.status( "orders" ).state == SessionState::Stopping );
                    REQUIRE( sessions.status( "orders" ).detail == "stopping…" );
                    REQUIRE( sessions.isStopping( "orders" ) );
                    REQUIRE_FALSE( sessions.isRunning( "orders" ) );
                    REQUIRE_FALSE( sessions.anyRunning() );
                    auto again = profile( "orders", server.url(), "orders.>" );
                    REQUIRE_FALSE( sessions.start( again ) );
                    REQUIRE( sessions.status( "orders" ).state == SessionState::Stopping );
                    REQUIRE( host.notifications.size() == 1 ); // started; stopped comes later
                }

                THEN( "once it has stopped, a notification says so, and no other tab is opened" )
                {
                    REQUIRE( waitFor( [ & ] {
                        return sessions.status( "orders" ).state == SessionState::Stopped;
                    } ) );
                    REQUIRE_FALSE( sessions.isStopping( "orders" ) );
                    REQUIRE( host.notifications.size() == 2 );
                    REQUIRE( host.notifications.last() == "NATS capture of orders.> stopped." );
                    REQUIRE( host.followedTabs.size() == 1 );
                }
            }

            AND_WHEN( "it is subscribed, and receives 5 messages within a second" )
            {
                REQUIRE( waitFor(
                    [ & ] { return sessions.status( "orders" ).state == SessionState::Running; },
                    10000 ) );
                {
                    Publisher publisher( server.url() );
                    for ( int i = 0; i < 5; ++i ) {
                        publisher.publish( "orders.eu", "x" );
                    }
                }
                REQUIRE( waitFor( [ & ] {
                    sessions.sample();
                    return sessions.status( "orders" ).received == 5;
                } ) );
                *nowMs += 1000;
                sessions.sample();

                THEN( "it runs, connected to the server, at 5 msgs/s" )
                {
                    const auto status = sessions.status( "orders" );
                    REQUIRE( status.state == SessionState::Running );
                    REQUIRE( status.server == server.url() );
                    REQUIRE( status.received == 5 );
                    REQUIRE( status.dropped == 0 );
                    REQUIRE( status.rate == 5 );
                }

                AND_WHEN( "nothing arrives in the next half second" )
                {
                    *nowMs += 500;
                    sessions.sample();

                    THEN( "the rate is 0" )
                    {
                        REQUIRE( sessions.status( "orders" ).rate == 0 );
                    }
                }

                AND_WHEN( "it is stopped without a notification" )
                {
                    sessions.stop( "orders", false );

                    THEN( "it is stopped, what it captured kept, and each transition was "
                          "announced once" )
                    {
                        REQUIRE( waitFor( [ & ] {
                            return sessions.status( "orders" ).state == SessionState::Stopped;
                        } ) );
                        REQUIRE( host.notifications.size() == 1 ); // started

                        const auto status = sessions.status( "orders" );
                        REQUIRE( status.state == SessionState::Stopped );
                        REQUIRE( status.detail == "stopped" );
                        REQUIRE( status.server.isEmpty() );
                        REQUIRE( status.received == 5 );
                        REQUIRE( status.rate == 0 );
                        REQUIRE_FALSE( sessions.isRunning( "orders" ) );
                        REQUIRE( announced.states()
                                 == QList<SessionState>{
                                     SessionState::Connecting, SessionState::Running,
                                     SessionState::Stopping, SessionState::Stopped } );
                    }
                }
            }
        }

        WHEN( "a connection to where no server listens is started" )
        {
            int port = 0;
            {
                NatsServer gone; // a port that was free a moment ago
                port = gone.port();
            }
            const auto url = QStringLiteral( "nats://127.0.0.1:%1" ).arg( port );
            auto orders = profile( "orders", url, "x" );
            REQUIRE( sessions.start( orders ) );

            THEN( "it failed, saying why, announced once, and no longer runs" )
            {
                REQUIRE( waitFor(
                    [ & ] { return sessions.status( "orders" ).state == SessionState::Failed; },
                    10000 ) );
                const auto status = sessions.status( "orders" );
                INFO( status.detail.toStdString() );
                REQUIRE( status.detail.startsWith( "connection to " + url + " failed: " ) );
                REQUIRE_FALSE( sessions.isRunning( "orders" ) );
                REQUIRE( announced.states()
                         == QList<SessionState>{ SessionState::Connecting, SessionState::Failed } );

                AND_THEN( "a notification says why, and the host log has it as a warning" )
                {
                    const auto reason = "NATS Monitor (capture of x): " + status.detail;
                    REQUIRE( host.notifications.size() == 2 );
                    REQUIRE( host.notifications.last() == reason );
                    REQUIRE( host.logged( LOGSQUIRL_LOG_WARNING ) == QStringList{ reason } );
                }
            }
        }

        WHEN( "a connection with a size limit of 1 MiB captures more than that" )
        {
            auto orders = profile( "orders", server.url(), "orders.>" );
            orders.maxFileMiB = 1;
            REQUIRE( sessions.start( orders ) );
            REQUIRE( waitFor(
                [ & ] { return sessions.status( "orders" ).state == SessionState::Running; },
                10000 ) );
            {
                Publisher publisher( server.url() );
                const QByteArray payload( 64 * 1024, 'x' );
                for ( int i = 0; i < 20; ++i ) {
                    publisher.publish( "orders.eu", payload );
                }
            }

            THEN( "the next file is opened in a second followed tab, and the host log says "
                  "why" )
            {
                REQUIRE( waitFor( [ & ] { return host.followedTabs.size() == 2; } ) );
                const auto status = sessions.status( "orders" );
                REQUIRE( status.files == 2 );
                REQUIRE( host.followedTabs.last() == status.filePath );
                REQUIRE( host.logged( LOGSQUIRL_LOG_INFO )
                         == QStringList{ "NATS Monitor: the capture of orders.> reached its size "
                                         "limit of 1 MiB; it goes on in "
                                         + status.filePath + "." } );
                REQUIRE( host.notifications.size() == 1 ); // started, not rotated
            }
        }
    }
}

SCENARIO( "a connection that cannot start has failed, saying why", "[sessions]" )
{
    GIVEN( "a connection without a subject" )
    {
        MemoryHost host;
        Sessions sessions( host, sessionTempRoot() );
        Announced announced( sessions, "x" );

        THEN( "it does not start, and its status says why, announced once; no tab is opened "
              "and nothing notified" )
        {
            auto x = profile( "x", "nats://127.0.0.1:1", "" );
            REQUIRE_FALSE( sessions.start( x ) );
            const auto status = sessions.status( "x" );
            REQUIRE( status.state == SessionState::Failed );
            REQUIRE( status.detail == "Enter the subject to capture." );
            REQUIRE_FALSE( sessions.isRunning( "x" ) );
            REQUIRE( announced.states() == QList<SessionState>{ SessionState::Failed } );
            REQUIRE( host.followedTabs.isEmpty() );
            REQUIRE( host.notifications.isEmpty() );
        }
    }

    GIVEN( "Sessions whose temporary root cannot hold a directory" )
    {
        QTemporaryDir parent;
        REQUIRE( parent.isValid() );
        MemoryHost host;
        Sessions sessions( host, parent.filePath( "missing/root" ) );
        Announced announced( sessions, "x" );

        THEN( "the session refuses to start, and the status says why, announced once" )
        {
            auto x = profile( "x", "nats://127.0.0.1:1", "x" );
            REQUIRE_FALSE( sessions.start( x ) );
            const auto status = sessions.status( "x" );
            INFO( status.detail.toStdString() );
            REQUIRE( status.state == SessionState::Failed );
            REQUIRE( status.detail.startsWith( "Cannot create a temporary directory" ) );
            REQUIRE_FALSE( sessions.isRunning( "x" ) );
            REQUIRE( announced.states() == QList<SessionState>{ SessionState::Failed } );

            AND_THEN( "no tab is opened, and a notification says why" )
            {
                REQUIRE( host.followedTabs.isEmpty() );
                REQUIRE( host.notifications
                         == QStringList{ "NATS Monitor (capture of x): " + status.detail } );
            }
        }
    }
}

SCENARIO( "the profile a session is started with no longer holds its password", "[sessions]" )
{
    GIVEN( "Sessions, and a connection that logs in as alice with the password s3cret" )
    {
        MemoryHost host;
        Sessions sessions( host, sessionTempRoot() );
        auto alice = profile( "x", "nats://127.0.0.1:1", "x" );
        alice.auth.method = nats_monitor::AuthMethod::UserPassword;
        alice.auth.user = "alice";
        alice.auth.secret = "s3cret";

        WHEN( "it is started" )
        {
            REQUIRE( sessions.start( alice ) );

            THEN( "the profile no longer holds the password, only the user" )
            {
                REQUIRE( alice.auth.secret.isEmpty() );
                REQUIRE( alice.auth.user == "alice" );
            }
            sessions.stopAll();
        }

        WHEN( "it is started while its session runs already" )
        {
            auto again = alice;
            REQUIRE( sessions.start( alice ) );
            REQUIRE( sessions.start( again ) );

            THEN( "neither profile holds the password" )
            {
                REQUIRE( alice.auth.secret.isEmpty() );
                REQUIRE( again.auth.secret.isEmpty() );
            }
            sessions.stopAll();
        }

        WHEN( "it cannot start" )
        {
            alice.subjects.clear();
            REQUIRE_FALSE( sessions.start( alice ) );

            THEN( "the profile no longer holds the password either" )
            {
                REQUIRE( alice.auth.secret.isEmpty() );
            }
        }
    }
}

SCENARIO( "Sessions tells the host when it does not verify a server's certificate", "[sessions]" )
{
    GIVEN( "Sessions on an in-memory host, and a connection with TLS that does not verify "
           "the certificate" )
    {
        MemoryHost host;
        Sessions sessions( host, sessionTempRoot() );
        auto insecure = profile( "x", "nats://127.0.0.1:1", "x" );
        insecure.tls.enabled = true;
        insecure.tls.skipVerify = true;

        WHEN( "it is started" )
        {
            REQUIRE( sessions.start( insecure ) );

            THEN( "the host log warns of it first, and its file is opened in one followed tab" )
            {
                REQUIRE_FALSE( host.logs.isEmpty() );
                REQUIRE( host.logs.first().level == LOGSQUIRL_LOG_WARNING );
                REQUIRE( host.logs.first().message
                         == "NATS Monitor: the certificate of nats://127.0.0.1:1 is not verified "
                            "(insecure)." );
                REQUIRE( host.followedTabs == QStringList{ sessions.status( "x" ).filePath } );
            }
            sessions.stopAll();
        }
    }
}

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
 * @file sessionstatus_test.cpp
 * @brief BDD tests for the status of a connection: the state its session's
 *        transitions lead to, its counts and its rate (issue #29).
 *
 * Without a server and without a clock: the transitions are made here, and
 * each sample is given its time.
 */

#include <catch2/catch.hpp>

#include "sessionstatus.h"

using nats_monitor::ConnectionStatus;
using nats_monitor::SessionState;
using Kind = nats_monitor::Transition::Kind;

namespace {

/// A connection whose session has started on nats://a:4222, writing @p path.
ConnectionStatus started( const QString& path = QStringLiteral( "/tmp/nats_x.log" ) )
{
    ConnectionStatus status;
    status.apply( { Kind::Connecting, "connecting to nats://a:4222…", {} } );
    status.opened( path );
    return status;
}

} // namespace

SCENARIO( "a connection's state follows its session's transitions", "[status]" )
{
    GIVEN( "a connection never started" )
    {
        ConnectionStatus status;

        THEN( "it is stopped, with nothing to say" )
        {
            REQUIRE( status.status().state == SessionState::Stopped );
            REQUIRE( status.status().detail.isEmpty() );
            REQUIRE( status.status().filePath.isEmpty() );
            REQUIRE( status.status().files == 0 );
        }
    }

    GIVEN( "a session that has started" )
    {
        auto status = started();

        THEN( "it connects, saying where to, and writes its first file" )
        {
            REQUIRE( status.status().state == SessionState::Connecting );
            REQUIRE( status.status().detail == "connecting to nats://a:4222…" );
            REQUIRE( status.status().server.isEmpty() );
            REQUIRE( status.status().filePath == "/tmp/nats_x.log" );
            REQUIRE( status.status().files == 1 );
        }

        WHEN( "it runs" )
        {
            status.apply( { Kind::Running, "connected to nats://a:4222", "nats://a:4222" } );

            THEN( "it is running, connected to that server" )
            {
                REQUIRE( status.status().state == SessionState::Running );
                REQUIRE( status.status().server == "nats://a:4222" );
            }

            AND_WHEN( "its connection breaks" )
            {
                status.apply( { Kind::Reconnecting, "disconnected", {} } );

                THEN( "it connects again, to no server for now, saying why" )
                {
                    REQUIRE( status.status().state == SessionState::Connecting );
                    REQUIRE( status.status().server.isEmpty() );
                    REQUIRE( status.status().detail == "disconnected" );
                }

                AND_WHEN( "it is connected again, to another server of the cluster" )
                {
                    status.apply(
                        { Kind::Running, "reconnected to nats://b:4222", "nats://b:4222" } );

                    THEN( "it runs, connected to that one" )
                    {
                        REQUIRE( status.status().state == SessionState::Running );
                        REQUIRE( status.status().server == "nats://b:4222" );
                    }
                }
            }

            AND_WHEN( "it reports an error and runs on" )
            {
                status.reported( "error: Permissions Violation" );

                THEN( "it still runs, the error its detail" )
                {
                    REQUIRE( status.status().state == SessionState::Running );
                    REQUIRE( status.status().detail == "error: Permissions Violation" );
                }
            }

            AND_WHEN( "its file is rotated twice" )
            {
                status.rotated( "/tmp/nats_x.2.log" );
                status.rotated( "/tmp/nats_x.3.log" );

                THEN( "it writes the last, its third file" )
                {
                    REQUIRE( status.status().filePath == "/tmp/nats_x.3.log" );
                    REQUIRE( status.status().files == 3 );
                }
            }
        }
    }

    GIVEN( "a running session that has a rate" )
    {
        auto status = started();
        status.apply( { Kind::Running, "connected to nats://a:4222", "nats://a:4222" } );
        status.sample( 10, 2, 1000 );
        status.sample( 30, 2, 2000 );
        const auto [ kind, detail, state ] = GENERATE( table<Kind, QString, SessionState>( {
            { Kind::Failed, "connection closed", SessionState::Failed },
            { Kind::Completed, "replay complete", SessionState::Stopped },
            { Kind::Stopped, "stopped", SessionState::Stopped },
        } ) );

        WHEN( "it ends: " << detail.toStdString() )
        {
            status.apply( { kind, detail, {} } );

            THEN( "the state says how, the detail why, and what it captured is kept" )
            {
                REQUIRE( status.status().state == state );
                REQUIRE( status.status().detail == detail );
                REQUIRE( status.status().server.isEmpty() );
                REQUIRE( status.status().filePath == "/tmp/nats_x.log" );
                REQUIRE( status.status().received == 30 );
                REQUIRE( status.status().dropped == 2 );
                REQUIRE( status.status().rate == 0 );
            }

            AND_WHEN( "it is started again" )
            {
                status.apply( { Kind::Connecting, "connecting to nats://a:4222…", {} } );

                THEN( "it starts afresh: no file, nothing counted" )
                {
                    REQUIRE( status.status().state == SessionState::Connecting );
                    REQUIRE( status.status().filePath.isEmpty() );
                    REQUIRE( status.status().files == 0 );
                    REQUIRE( status.status().received == 0 );
                    REQUIRE( status.status().dropped == 0 );
                }
            }
        }
    }

    GIVEN( "a session that could not start" )
    {
        ConnectionStatus status;
        status.apply( { Kind::Failed, "Enter the subject to capture.", {} } );

        THEN( "it failed, saying why, without a file" )
        {
            REQUIRE( status.status().state == SessionState::Failed );
            REQUIRE( status.status().detail == "Enter the subject to capture." );
            REQUIRE( status.status().filePath.isEmpty() );
        }
    }
}

SCENARIO( "a connection's rate is what its session received per second between two samples",
          "[status]" )
{
    GIVEN( "a running session, first sampled at 5 s" )
    {
        auto status = started();
        status.apply( { Kind::Running, "connected to nats://a:4222", "nats://a:4222" } );
        status.sample( 0, 0, 5000 );

        THEN( "it has no rate yet" )
        {
            REQUIRE( status.status().rate == 0 );
        }

        WHEN( "it received 1,234 by 6 s, 3 of them dropped" )
        {
            status.sample( 1234, 3, 6000 );

            THEN( "the rate is 1,234 msgs/s, and the counts are those" )
            {
                REQUIRE( status.status().rate == 1234 );
                REQUIRE( status.status().received == 1234 );
                REQUIRE( status.status().dropped == 3 );
            }

            AND_WHEN( "another 250 arrive in the next half second" )
            {
                status.sample( 1484, 3, 6500 );

                THEN( "the rate is 500 msgs/s" )
                {
                    REQUIRE( status.status().rate == 500 );
                }
            }

            AND_WHEN( "it is sampled again at the same time, with more received" )
            {
                status.sample( 2000, 3, 6000 );

                THEN( "the rate stays, the counts are the new ones" )
                {
                    REQUIRE( status.status().rate == 1234 );
                    REQUIRE( status.status().received == 2000 );
                }

                AND_WHEN( "a second later" )
                {
                    status.sample( 2100, 3, 7000 );

                    THEN( "the rate counts from the last sample at an earlier time" )
                    {
                        REQUIRE( status.status().rate == 866 );
                    }
                }
            }

            AND_WHEN( "nothing arrives in the next second" )
            {
                status.sample( 1234, 3, 7000 );

                THEN( "the rate is 0" )
                {
                    REQUIRE( status.status().rate == 0 );
                }
            }
        }
    }

    GIVEN( "a session started again after it had a rate" )
    {
        auto status = started();
        status.sample( 0, 0, 1000 );
        status.sample( 500, 0, 2000 );
        REQUIRE( status.status().rate == 500 );
        status.apply( { Kind::Stopped, "stopped", {} } );
        status.apply( { Kind::Connecting, "connecting to nats://a:4222…", {} } );

        WHEN( "it is sampled, counting from 0 again" )
        {
            status.sample( 40, 0, 3000 );

            THEN( "the rate only starts there" )
            {
                REQUIRE( status.status().rate == 0 );
                REQUIRE( status.status().received == 40 );
            }
        }
    }
}

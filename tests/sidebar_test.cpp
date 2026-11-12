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
 * @file sidebar_test.cpp
 * @brief BDD tests for what a tile shows of its connection's status
 *        (issue #29).
 *
 * The sidebar only maps the status Sessions keeps onto the tile: its state
 * and its details, tested here without a session.  The numbers are in the
 * locale's format, as the tile shows them.
 */

#include <catch2/catch.hpp>

#include "sidebarwidget.h"

#include <QFile>
#include <QLocale>
#include <QTemporaryDir>

using nats_monitor::SessionState;
using nats_monitor::SessionStatus;
using nats_monitor::TileState;
using nats_monitor::tileViewOf;

namespace {

/// @p n in the locale's format.
QString number( quint64 n )
{
    return QLocale().toString( n );
}

} // namespace

SCENARIO( "a tile shows its connection's status", "[sidebar]" )
{
    GIVEN( "a connection never started" )
    {
        const auto view = tileViewOf( SessionStatus{} );

        THEN( "its tile is stopped, without details" )
        {
            REQUIRE( view.state == TileState::Stopped );
            REQUIRE( view.details.isEmpty() );
        }
    }

    GIVEN( "a capture whose file holds 2 KiB" )
    {
        QTemporaryDir dir;
        REQUIRE( dir.isValid() );
        const auto path = dir.filePath( "nats_orders.all.log" );
        {
            QFile file( path );
            REQUIRE( file.open( QIODevice::WriteOnly ) );
            file.write( QByteArray( 2048, 'x' ) );
        }
        const auto fileLine
            = "nats_orders.all.log · "
              + QLocale().formattedDataSize( 2048, 1, QLocale::DataSizeIecFormat );
        SessionStatus status;
        status.filePath = path;
        status.files = 1;
        status.received = 12345;
        status.dropped = 7;
        const auto counts = "received " + number( 12345 ) + " · dropped " + number( 7 );

        WHEN( "it runs at 1,234 msgs/s" )
        {
            status.state = SessionState::Running;
            status.server = "nats://a:4222";
            status.detail = "connected to nats://a:4222";
            status.rate = 1234;
            const auto view = tileViewOf( status );

            THEN( "the tile runs, with the rate, the counts, the file and the server" )
            {
                REQUIRE( view.state == TileState::Running );
                REQUIRE( view.details
                         == QStringList{ "running · " + number( 1234 ) + " msgs/s", counts,
                                         fileLine, "connected to nats://a:4222" } );
            }
        }

        WHEN( "it has rotated into its third file" )
        {
            status.state = SessionState::Running;
            status.server = "nats://a:4222";
            status.files = 3;

            THEN( "the file's line says so" )
            {
                REQUIRE( tileViewOf( status ).details.at( 2 ) == fileLine + " · file 3" );
            }
        }

        WHEN( "it connects again" )
        {
            status.state = SessionState::Connecting;
            status.detail = "disconnected";

            THEN( "the tile connects, saying the last event, with the counts and the file" )
            {
                const auto view = tileViewOf( status );
                REQUIRE( view.state == TileState::Connecting );
                REQUIRE( view.details
                         == QStringList{ "connecting · disconnected", counts, fileLine } );
            }
        }

        WHEN( "it was stopped, or its replay is complete" )
        {
            status.state = SessionState::Stopped;
            status.detail = GENERATE( QStringLiteral( "stopped" ),
                                      QStringLiteral( "replay complete" ) );

            THEN( "the tile is stopped, saying how, with what it captured" )
            {
                const auto view = tileViewOf( status );
                REQUIRE( view.state == TileState::Stopped );
                REQUIRE( view.details == QStringList{ status.detail, counts, fileLine } );
            }
        }

        WHEN( "it failed" )
        {
            status.state = SessionState::Failed;
            status.detail = "connection closed";

            THEN( "the tile failed, saying why, with what it captured" )
            {
                const auto view = tileViewOf( status );
                REQUIRE( view.state == TileState::Failed );
                REQUIRE( view.details
                         == QStringList{ "failed: connection closed", counts, fileLine } );
            }
        }
    }

    GIVEN( "a connection that could not start" )
    {
        SessionStatus status;
        status.state = SessionState::Failed;
        status.detail = "Enter the subject to capture.";

        THEN( "the tile failed, saying why, and nothing more" )
        {
            const auto view = tileViewOf( status );
            REQUIRE( view.state == TileState::Failed );
            REQUIRE( view.details == QStringList{ "failed: Enter the subject to capture." } );
        }
    }
}

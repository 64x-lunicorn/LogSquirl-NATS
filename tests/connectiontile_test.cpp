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
 * @file connectiontile_test.cpp
 * @brief BDD tests for the tile of a saved connection (issue #23).
 *
 * The tile is a plain widget that knows nothing of NATS, so it is tested on
 * its own, through its named children, as the sidebar sets it.
 */

#include <catch2/catch.hpp>

#include "connectiontile.h"

#include <QApplication>
#include <QColor>
#include <QEvent>
#include <QLabel>
#include <QPalette>
#include <QPushButton>

using nats_monitor::ConnectionTile;
using nats_monitor::TileState;

namespace {

QString stateOf( const ConnectionTile& tile )
{
    return tile.findChild<QLabel*>( "state" )->property( "state" ).toString();
}

QPushButton* button( const ConnectionTile& tile, const char* name )
{
    return tile.findChild<QPushButton*>( name );
}

QWidget* child( const ConnectionTile& tile, const char* name )
{
    return tile.findChild<QWidget*>( name );
}

} // namespace

SCENARIO( "a tile shows by a dot whether its connection runs", "[tile]" )
{
    GIVEN( "a new tile" )
    {
        ConnectionTile tile( "prod" );
        tile.show();

        THEN( "it is stopped: a grey dot, Start, Delete shown, no details" )
        {
            REQUIRE( stateOf( tile ) == "stopped" );
            REQUIRE( button( tile, "startStop" )->text().contains( "Start" ) );
            REQUIRE( button( tile, "delete" )->isVisible() );
            REQUIRE_FALSE( child( tile, "details" )->isVisible() );
            REQUIRE_FALSE( child( tile, "separator" )->isVisible() );
        }

        WHEN( "it runs" )
        {
            tile.setState( TileState::Running );

            THEN( "the dot says so, Stop, and Delete is hidden" )
            {
                REQUIRE( stateOf( tile ) == "running" );
                REQUIRE( button( tile, "startStop" )->text().contains( "Stop" ) );
                REQUIRE_FALSE( button( tile, "delete" )->isVisible() );
                REQUIRE( tile.property( "running" ).toBool() );
            }
        }

        WHEN( "it connects" )
        {
            tile.setState( TileState::Connecting );

            THEN( "the dot says so, and it can be stopped, not deleted" )
            {
                REQUIRE( stateOf( tile ) == "connecting" );
                REQUIRE( button( tile, "startStop" )->text().contains( "Stop" ) );
                REQUIRE_FALSE( button( tile, "delete" )->isVisible() );
            }
        }

        WHEN( "it is stopping" )
        {
            tile.setState( TileState::Stopping );

            THEN( "the dot says so, and it can neither be started, stopped again, edited nor "
                  "deleted" )
            {
                REQUIRE( stateOf( tile ) == "stopping" );
                REQUIRE( button( tile, "startStop" )->text().contains( "Stopping" ) );
                REQUIRE_FALSE( button( tile, "startStop" )->isEnabled() );
                REQUIRE_FALSE( button( tile, "edit" )->isEnabled() );
                REQUIRE_FALSE( button( tile, "delete" )->isVisible() );
                REQUIRE( tile.property( "running" ).toBool() );
            }

            AND_WHEN( "it has stopped" )
            {
                tile.setState( TileState::Stopped );

                THEN( "it can be started, edited and deleted again" )
                {
                    REQUIRE( button( tile, "startStop" )->text().contains( "Start" ) );
                    REQUIRE( button( tile, "startStop" )->isEnabled() );
                    REQUIRE( button( tile, "edit" )->isEnabled() );
                    REQUIRE( button( tile, "delete" )->isVisible() );
                }
            }
        }

        WHEN( "it failed" )
        {
            tile.setState( TileState::Failed );

            THEN( "the dot says so, and it can be started again or deleted" )
            {
                REQUIRE( stateOf( tile ) == "failed" );
                REQUIRE( button( tile, "startStop" )->text().contains( "Start" ) );
                REQUIRE( button( tile, "delete" )->isVisible() );
            }
        }

        WHEN( "it has details" )
        {
            tile.setDetails( { "running · 0 msgs/s", "received 19 · dropped 0" } );

            THEN( "they are shown below a line; without any, both are hidden again" )
            {
                REQUIRE( child( tile, "details" )->isVisible() );
                REQUIRE( child( tile, "separator" )->isVisible() );
                tile.setDetails( {} );
                REQUIRE_FALSE( child( tile, "details" )->isVisible() );
                REQUIRE_FALSE( child( tile, "separator" )->isVisible() );
            }
        }
    }
}

SCENARIO( "a tile takes its colours from the palette around it", "[tile]" )
{
    GIVEN( "a tile in a dark sidebar" )
    {
        QWidget sidebar;
        QPalette dark;
        dark.setColor( QPalette::Window, QColor( 30, 30, 30 ) );
        dark.setColor( QPalette::WindowText, QColor( 230, 230, 230 ) );
        sidebar.setPalette( dark );
        auto* tile = new ConnectionTile( "prod", &sidebar );
        const auto styled = tile->styleSheet();

        THEN( "its card is dark too" )
        {
            REQUIRE(
                styled.contains( "background: " + QColor( 30, 30, 30 ).lighter( 125 ).name() ) );
        }

        WHEN( "its own palette changes again and again" )
        {
            for ( int i = 0; i < 5; ++i ) {
                QEvent change( QEvent::PaletteChange );
                QApplication::sendEvent( tile, &change );
            }

            THEN( "its colours stay as they were" )
            {
                REQUIRE( tile->styleSheet() == styled );
            }
        }

        WHEN( "the sidebar turns light" )
        {
            QPalette light;
            light.setColor( QPalette::Window, QColor( 236, 236, 236 ) );
            light.setColor( QPalette::WindowText, QColor( 20, 20, 20 ) );
            sidebar.setPalette( light );

            THEN( "the card follows" )
            {
                REQUIRE( tile->styleSheet().contains(
                    "background: " + QColor( 236, 236, 236 ).darker( 103 ).name() ) );
            }
        }
    }
}

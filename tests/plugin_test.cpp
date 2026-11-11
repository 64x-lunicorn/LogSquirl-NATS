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
 * @file plugin_test.cpp
 * @brief BDD tests for the plugin's lifecycle through its C entry points.
 *
 * Drives logsquirl_plugin_init() / logsquirl_plugin_shutdown() against a
 * FakeHost, and clicks the menu entry the plugin registers.
 */

#include <catch2/catch.hpp>

#include "fakehost.h"

using nats_test::FakeHost;

extern "C" int logsquirl_plugin_init( const LogSquirlHostApi* api, void* handle );
extern "C" void logsquirl_plugin_shutdown( void );
extern "C" void logsquirl_plugin_configure( void* parent_widget );

SCENARIO( "the plugin adds NATS Monitor to the Plugins menu", "[plugin]" )
{
    GIVEN( "a host" )
    {
        FakeHost host;

        WHEN( "the plugin is initialised" )
        {
            const int result = logsquirl_plugin_init( host.api(), host.handle() );

            THEN( "it succeeds and registers one menu entry, NATS Monitor…" )
            {
                REQUIRE( result == 0 );
                REQUIRE( host.menuActions.size() == 1 );
                REQUIRE( host.menuActions.first().menuPath == "Plugins" );
                REQUIRE( host.menuActions.first().label == QString::fromUtf8( "NATS Monitor…" ) );
            }

            AND_WHEN( "the user clicks the entry" )
            {
                REQUIRE( host.menuActions.size() == 1 );
                host.menuActions.first().trigger();

                THEN( "nothing happens yet, and nothing fails" )
                {
                    REQUIRE( host.openedFiles.isEmpty() );
                    REQUIRE( host.notifications.isEmpty() );
                    REQUIRE_FALSE( host.logs.join( '\n' ).contains( "failed" ) );
                }
            }

            logsquirl_plugin_shutdown();
        }
    }
}

SCENARIO( "a failure during initialisation never reaches the host", "[plugin]" )
{
    GIVEN( "a host whose register_menu_action throws" )
    {
        FakeHost host;
        host.failMenuAction = true;

        WHEN( "the plugin is initialised" )
        {
            int result = 0;
            REQUIRE_NOTHROW( result = logsquirl_plugin_init( host.api(), host.handle() ) );

            THEN( "init reports the failure and logs why" )
            {
                REQUIRE( result != 0 );
                REQUIRE( host.logs.join( '\n' ).contains( "the menu is full" ) );
            }
        }
    }

    GIVEN( "no host API table" )
    {
        THEN( "init refuses" )
        {
            REQUIRE( logsquirl_plugin_init( nullptr, nullptr ) != 0 );
        }
    }
}

SCENARIO( "configure and shutdown are safe without a running session", "[plugin]" )
{
    GIVEN( "an initialised plugin" )
    {
        FakeHost host;
        REQUIRE( logsquirl_plugin_init( host.api(), host.handle() ) == 0 );

        THEN( "configure and shutdown do not throw" )
        {
            REQUIRE_NOTHROW( logsquirl_plugin_configure( nullptr ) );
            REQUIRE_NOTHROW( logsquirl_plugin_shutdown() );
        }
    }
}

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
 * @file library_test.cpp
 * @brief The built plugin library loads as the host loads it.
 *
 * LogSquirl opens the library with QLibrary and resolves the entry points
 * by name.  This loads the very library the package carries — with nats.c
 * and OpenSSL linked in — and registers its menu entry through a FakeHost.
 */

#include <catch2/catch.hpp>

#include "fakehost.h"

#include <QLibrary>

#include <cstring>

using nats_test::FakeHost;

SCENARIO( "the plugin library loads and adds its menu entry", "[library]" )
{
    GIVEN( "the built plugin library" )
    {
        QLibrary library( QStringLiteral( LOGSQUIRL_NATS_LIBRARY ) );

        WHEN( "the host loads it and resolves the entry points" )
        {
            INFO( library.errorString().toStdString() );
            REQUIRE( library.load() );

            using GetInfo = const LogSquirlPluginInfo* ( * )();
            using Init = int ( * )( const LogSquirlHostApi*, void* );
            using Shutdown = void ( * )();
            auto getInfo
                = reinterpret_cast<GetInfo>( library.resolve( "logsquirl_plugin_get_info" ) );
            auto init = reinterpret_cast<Init>( library.resolve( "logsquirl_plugin_init" ) );
            auto shutdown
                = reinterpret_cast<Shutdown>( library.resolve( "logsquirl_plugin_shutdown" ) );
            REQUIRE( getInfo );
            REQUIRE( init );
            REQUIRE( shutdown );
            REQUIRE( library.resolve( "logsquirl_plugin_configure" ) );

            THEN( "it describes itself and registers NATS Monitor… on init" )
            {
                REQUIRE( std::strcmp( getInfo()->id, "io.github.logsquirl.nats" ) == 0 );

                FakeHost host;
                REQUIRE( init( host.api(), host.handle() ) == 0 );
                REQUIRE( host.menuActions.size() == 1 );
                REQUIRE( host.menuActions.first().label == QString::fromUtf8( "NATS Monitor…" ) );
                // The ready message names the nats.c and OpenSSL inside.
                REQUIRE( host.logs.join( '\n' ).contains( "OpenSSL 3." ) );
                shutdown();
            }

            library.unload();
        }
    }
}

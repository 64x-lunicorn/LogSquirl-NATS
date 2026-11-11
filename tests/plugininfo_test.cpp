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
 * @file plugininfo_test.cpp
 * @brief BDD tests verifying the plugin's C ABI metadata.
 *
 * logsquirl_plugin_get_info() must describe the plugin as plugin.json does,
 * and the package must carry what plugin.json names.
 */

#include <catch2/catch.hpp>

#include "logsquirl_plugin_api.h"
#include "testconfig.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <cstring>

extern "C" const LogSquirlPluginInfo* logsquirl_plugin_get_info( void );

SCENARIO( "logsquirl_plugin_get_info returns valid metadata", "[plugininfo]" )
{
    GIVEN( "the exported get_info function" )
    {
        const auto* info = logsquirl_plugin_get_info();
        REQUIRE( info != nullptr );

        THEN( "it names the plugin as plugin.json does" )
        {
            REQUIRE( std::strcmp( info->id, "io.github.logsquirl.nats" ) == 0 );
            REQUIRE( std::strcmp( info->name, "NATS Monitor" ) == 0 );
            REQUIRE( std::strcmp( info->version, "0.1.0" ) == 0 );
            REQUIRE( std::strcmp( info->license, "GPL-3.0-or-later" ) == 0 );
        }

        THEN( "it is a UI plugin of the host's API version" )
        {
            REQUIRE( info->type == LOGSQUIRL_PLUGIN_UI );
            REQUIRE( info->api_version == 1 );
        }
    }
}

SCENARIO( "the package carries the Log Format", "[plugininfo]" )
{
    const auto readJson = []( const QString& relativePath ) {
        QFile file( QStringLiteral( LOGSQUIRL_NATS_TEST_SOURCE_DIR "/" ) + relativePath );
        REQUIRE( file.open( QIODevice::ReadOnly ) );
        return QJsonDocument::fromJson( file.readAll() ).object();
    };

    THEN( "CI packages formats/nats_log.json beside the library" )
    {
        const auto packageFiles = readJson( ".github/plugin-ci.json" ).value( "package_files" );
        REQUIRE( packageFiles.toArray().contains( QJsonValue( "formats/nats_log.json" ) ) );
    }

    THEN( "plugin.json names it, so LogSquirl 26.12 and later use it without an import" )
    {
        // The package is flat: the file sits beside plugin.json.
        const auto formats = readJson( "plugin.json" ).value( "formats" ).toArray();
        REQUIRE( formats == QJsonArray{ "nats_log.json" } );
    }
}

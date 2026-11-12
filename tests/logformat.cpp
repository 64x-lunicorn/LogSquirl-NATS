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

#include "logformat.h"

#include "testconfig.h"

#include <catch2/catch.hpp>

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>

namespace nats_test {

LogFormat loadLogFormat()
{
    QFile file( QStringLiteral( LOGSQUIRL_NATS_TEST_SOURCE_DIR "/formats/nats_log.json" ) );
    REQUIRE( file.open( QIODevice::ReadOnly ) );
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson( file.readAll(), &error );
    INFO( error.errorString().toStdString() );
    REQUIRE( document.isObject() );

    LogFormat format;
    const auto root = document.object();
    for ( auto it = root.begin(); it != root.end(); ++it ) {
        if ( !it.key().startsWith( '$' ) ) {
            REQUIRE( format.name.isEmpty() ); // one format in the file
            format.name = it.key();
            format.definition = it.value().toObject();
        }
    }
    REQUIRE_FALSE( format.name.isEmpty() );

    const auto& def = format.definition;
    const auto regex = def.value( "regex" ).toObject();
    for ( auto it = regex.begin(); it != regex.end(); ++it ) {
        const auto pattern = it.value().toObject().value( "pattern" ).toString();
        format.patterns << pattern;
        format.regexes << QRegularExpression( pattern ); // no options, as LogSquirl's
    }
    if ( def.contains( "timestamp-field" ) ) {
        format.timestampField = def.value( "timestamp-field" ).toString();
    }
    if ( def.contains( "body-field" ) ) {
        format.bodyField = def.value( "body-field" ).toString();
    }
    const auto tsFormat = def.value( "timestamp-format" );
    if ( tsFormat.isArray() ) {
        for ( const auto& f : tsFormat.toArray() ) {
            format.timestampFormats << f.toString();
        }
    }
    else if ( tsFormat.isString() ) {
        format.timestampFormats << tsFormat.toString();
    }
    return format;
}

const LogFormat& logFormat()
{
    static const LogFormat format = loadLogFormat();
    return format;
}

QRegularExpressionMatch matchLine( const LogFormat& format, const QString& line )
{
    for ( const auto& re : format.regexes ) {
        auto match = re.match( line );
        if ( match.hasMatch() ) {
            return match;
        }
    }
    return {};
}

QStringList namedGroups( const QString& pattern )
{
    static const QRegularExpression namedGroupRe( R"(\(\?<([a-zA-Z_]\w*)>)" );
    QStringList names;
    auto it = namedGroupRe.globalMatch( pattern );
    while ( it.hasNext() ) {
        names << it.next().captured( 1 );
    }
    return names;
}

} // namespace nats_test

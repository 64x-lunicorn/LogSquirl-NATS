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
 * @file lineformat.cpp
 * @brief Implementation of the line format.
 */

#include "lineformat.h"

#include <QByteArrayView>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>

namespace nats_monitor {

namespace {

constexpr char kHexDigits[] = "0123456789abcdef";

/// Append `yyyy-MM-dd HH:mm:ss.uuuuuuZ` for @p us microseconds since the epoch.
void appendTimestamp( QByteArray& out, qint64 us )
{
    qint64 seconds = us / 1000000;
    qint64 micros = us % 1000000;
    if ( micros < 0 ) { // before 1970
        micros += 1000000;
        --seconds;
    }
    const auto time = static_cast<std::time_t>( seconds );
    std::tm utc{};
#ifdef _WIN32
    gmtime_s( &utc, &time );
#else
    gmtime_r( &time, &utc );
#endif
    char buffer[ 32 ];
    const int length = std::snprintf( buffer, sizeof buffer, "%04d-%02d-%02d %02d:%02d:%02d.%06dZ",
                                      utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday, utc.tm_hour,
                                      utc.tm_min, utc.tm_sec, static_cast<int>( micros ) );
    out.append( buffer, length );
}

/// Append @p text with `\`, CR and LF escaped, and the other C0 control
/// characters but tab as `\xNN`.  With @p headerSyntax, also `;` and `]`.
void appendEscaped( QByteArray& out, QByteArrayView text, bool headerSyntax )
{
    for ( const char c : text ) {
        const auto byte = static_cast<unsigned char>( c );
        switch ( c ) {
        case '\\':
            out.append( "\\\\" );
            continue;
        case '\n':
            out.append( "\\n" );
            continue;
        case '\r':
            out.append( "\\r" );
            continue;
        case '\t':
            out.append( c );
            continue;
        case ';':
        case ']':
            if ( headerSyntax ) {
                out.append( '\\' );
            }
            out.append( c );
            continue;
        default:
            break;
        }
        if ( byte < 0x20 ) {
            out.append( "\\x" );
            out.append( "0123456789ABCDEF"[ byte >> 4 ] );
            out.append( "0123456789ABCDEF"[ byte & 0x0F ] );
        }
        else {
            out.append( c );
        }
    }
}

/// Append `hex:` and the first @p maxBytes bytes of @p data in hex.
void appendHex( QByteArray& out, QByteArrayView data, int maxBytes )
{
    out.append( "hex:" );
    const auto count = std::min<qsizetype>( data.size(), std::max( maxBytes, 0 ) );
    for ( qsizetype i = 0; i < count; ++i ) {
        const auto byte = static_cast<unsigned char>( data[ i ] );
        out.append( kHexDigits[ byte >> 4 ] );
        out.append( kHexDigits[ byte & 0x0F ] );
    }
}

QByteArray format( qint64 receivedUs, QByteArrayView subject, QByteArrayView reply,
                   const QList<std::pair<QByteArray, QByteArray>>& headers, QByteArrayView payload,
                   const FormatOptions& options )
{
    const bool text = payload.isValidUtf8();
    QByteArray line;
    line.reserve(
        64 + subject.size() + reply.size()
        + ( text ? payload.size() + payload.size() / 8
                 : 4 + 2 * std::min<qsizetype>( payload.size(), options.maxBinaryBytes ) ) );

    appendTimestamp( line, receivedUs );
    line.append( ' ' );
    line.append( subject );
    line.append( ' ' );
    if ( reply.isEmpty() ) {
        line.append( '-' );
    }
    else {
        line.append( reply );
    }
    line.append( ' ' );
    line.append( QByteArray::number( payload.size() ) );

    if ( !headers.isEmpty() ) {
        line.append( " [" );
        bool first = true;
        for ( const auto& [ key, value ] : headers ) {
            if ( !first ) {
                line.append( "; " );
            }
            first = false;
            appendEscaped( line, key, true );
            line.append( '=' );
            appendEscaped( line, value, true );
        }
        line.append( ']' );
    }

    if ( !payload.isEmpty() ) {
        line.append( ' ' );
        if ( text ) {
            appendEscaped( line, payload, false );
        }
        else {
            appendHex( line, payload, options.maxBinaryBytes );
        }
    }
    return line;
}

} // namespace

QByteArray formatLine( const Message& message, const FormatOptions& options )
{
    return format( message.receivedUs, message.subject, message.reply, message.headers,
                   message.payload, options );
}

QByteArray formatMarker( qint64 receivedUs, const QByteArray& text )
{
    return format( receivedUs, kMarkerSubject, {}, {}, text, {} );
}

qint64 nowUs()
{
    using namespace std::chrono;
    return duration_cast<microseconds>( system_clock::now().time_since_epoch() ).count();
}

} // namespace nats_monitor

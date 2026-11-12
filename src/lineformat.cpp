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

/// For binary payloads (`hex:`).
constexpr char kLowerHexDigits[] = "0123456789abcdef";
/// For `\xNN` escapes in text.
constexpr char kUpperHexDigits[] = "0123456789ABCDEF";

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

/// What is escaped beyond what every field escapes.
enum class Field {
    HeaderKey,   ///< `;`, `]` and `=`
    HeaderValue, ///< `;` and `]`
    Payload,     ///< a leading `[` or `hex:`
};

/// Append @p byte as `\xNN`.
void appendHexEscape( QByteArray& out, unsigned char byte )
{
    out.append( "\\x" );
    out.append( kUpperHexDigits[ byte >> 4 ] );
    out.append( kUpperHexDigits[ byte & 0x0F ] );
}

/// Append @p text with `\`, CR and LF escaped, the other C0 control
/// characters but tab as `\xNN`, and what @p field needs escaped.
void appendEscaped( QByteArray& out, QByteArrayView text, Field field )
{
    // A payload must not read as a header block or a binary payload: its
    // first byte becomes a \xNN escape.
    if ( field == Field::Payload
         && ( text.startsWith( '[' ) || text.startsWith( QByteArrayView( "hex:" ) ) ) ) {
        appendHexEscape( out, static_cast<unsigned char>( text.front() ) );
        text = text.sliced( 1 );
    }
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
            if ( field != Field::Payload ) {
                out.append( '\\' );
            }
            out.append( c );
            continue;
        case '=':
            if ( field == Field::HeaderKey ) {
                out.append( '\\' );
            }
            out.append( c );
            continue;
        default:
            break;
        }
        if ( byte < 0x20 ) {
            appendHexEscape( out, byte );
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
        out.append( kLowerHexDigits[ byte >> 4 ] );
        out.append( kLowerHexDigits[ byte & 0x0F ] );
    }
}

} // namespace

QByteArray formatLine( const Message& message, const FormatOptions& options )
{
    const auto& payload = message.payload;
    const bool text = payload.isValidUtf8();
    QByteArray line;
    line.reserve(
        64 + message.subject.size() + message.reply.size()
        + ( text ? payload.size() + payload.size() / 8
                 : 4 + 2 * std::min<qsizetype>( payload.size(), options.maxBinaryBytes ) ) );

    appendTimestamp( line, message.receivedUs );
    line.append( ' ' );
    line.append( message.subject );
    line.append( ' ' );
    if ( message.reply.isEmpty() ) {
        line.append( '-' );
    }
    else {
        line.append( message.reply );
    }
    line.append( ' ' );
    if ( message.streamSequence != 0 ) {
        line.append( '#' );
        line.append( QByteArray::number( message.streamSequence ) );
        line.append( ' ' );
    }
    line.append( QByteArray::number( payload.size() ) );

    if ( !message.headers.isEmpty() ) {
        line.append( " [" );
        bool first = true;
        for ( const auto& [ key, value ] : message.headers ) {
            if ( !first ) {
                line.append( "; " );
            }
            first = false;
            appendEscaped( line, key, Field::HeaderKey );
            line.append( '=' );
            appendEscaped( line, value, Field::HeaderValue );
        }
        line.append( ']' );
    }

    if ( !payload.isEmpty() ) {
        line.append( ' ' );
        if ( text ) {
            appendEscaped( line, payload, Field::Payload );
        }
        else {
            appendHex( line, payload, options.maxBinaryBytes );
        }
    }
    return line;
}

QByteArray formatMarker( qint64 receivedUs, const QByteArray& text )
{
    Message marker;
    marker.receivedUs = receivedUs;
    marker.subject = kMarkerSubject;
    marker.payload = text;
    return formatLine( marker );
}

QByteArray formatTimestamp( qint64 us )
{
    QByteArray timestamp;
    appendTimestamp( timestamp, us );
    return timestamp;
}

qint64 nowUs()
{
    using namespace std::chrono;
    return duration_cast<microseconds>( system_clock::now().time_since_epoch() ).count();
}

} // namespace nats_monitor

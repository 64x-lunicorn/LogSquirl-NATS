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

// First: on Windows nats.h (through sessiontest.h) includes winsock2.h,
// which must come before any windows.h.
#include "sessiontest.h"

#include "capture.h"

#include "lineformat.h"
#include "logformat.h"

#include <QRegularExpression>

#include <algorithm>

namespace nats_test {

LineFields markerFields( const QString& text )
{
    return { QString::fromLatin1( nats_monitor::kMarkerSubject ),
             QStringLiteral( "-" ),
             0,
             text.toUtf8().size(),
             {},
             text };
}

LineFields liveFields( const QString& subject, const QString& payload )
{
    return { subject, QStringLiteral( "-" ), 0, payload.toUtf8().size(), {}, payload };
}

CaptureLine CaptureLine::read( const QString& text )
{
    CaptureLine line;
    line.text_ = text;
    const auto& format = logFormat();
    const auto match = matchLine( format, text );
    if ( !match.hasMatch() ) {
        return line;
    }
    const auto spanOf = [ &match ]( const char* name ) {
        const QLatin1String group( name );
        const auto start = match.capturedStart( group );
        return start < 0 ? Span{}
                         : Span{ static_cast<int>( start ),
                                 static_cast<int>( match.capturedLength( group ) ) };
    };
    line.timestamp_ = spanOf( "timestamp" );
    line.subject_ = spanOf( "subject" );
    line.reply_ = spanOf( "reply" );
    line.body_ = spanOf( format.bodyField.toLatin1().constData() );
    line.sequence_ = match.capturedView( QLatin1String( "seq" ) ).toULongLong();
    line.size_ = match.capturedView( QLatin1String( "size" ) ).toLongLong();

    // The body: the header block, `[k=v; k=v]` with `]` in it escaped by a
    // backslash, then a space and the payload; a payload never starts with
    // `[` (it is escaped as \x5B), so a body that does starts with headers.
    const auto body = line.span( line.body_ );
    if ( body.startsWith( '[' ) ) {
        int i = 1;
        while ( i < body.size() && body.at( i ) != ']' ) {
            i += body.at( i ) == '\\' ? 2 : 1;
        }
        if ( i >= body.size() ) {
            return line; // the header block is not closed
        }
        line.headers_ = { line.body_.start + 1, i - 1 };
        const int rest = i + 1;
        if ( rest < body.size() ) {
            if ( body.at( rest ) != ' ' ) {
                return line; // something else right after the header block
            }
            line.payload_
                = { line.body_.start + rest + 1, static_cast<int>( body.size() ) - rest - 1 };
        }
    }
    else {
        line.payload_ = line.body_;
    }
    line.kind_ = line.span( line.subject_ ) == QLatin1String( nats_monitor::kMarkerSubject )
                     ? Kind::Marker
                     : Kind::Message;
    return line;
}

QString CaptureLine::span( Span s ) const
{
    return text_.mid( s.start, s.length );
}

QString CaptureLine::timestamp() const
{
    return kind_ == Kind::Unreadable ? QString() : span( timestamp_ );
}

QString CaptureLine::subject() const
{
    return kind_ == Kind::Unreadable ? QString() : span( subject_ );
}

QString CaptureLine::reply() const
{
    return kind_ == Kind::Unreadable ? QString() : span( reply_ );
}

quint64 CaptureLine::sequence() const
{
    return kind_ == Kind::Unreadable ? 0 : sequence_;
}

qint64 CaptureLine::size() const
{
    return kind_ == Kind::Unreadable ? 0 : size_;
}

QString CaptureLine::body() const
{
    return kind_ == Kind::Unreadable ? QString() : span( body_ );
}

QString CaptureLine::headers() const
{
    return kind_ == Kind::Unreadable ? QString() : span( headers_ );
}

QString CaptureLine::payload() const
{
    return kind_ == Kind::Unreadable ? QString() : span( payload_ );
}

QString CaptureLine::markerText() const
{
    return isMarker() ? span( payload_ ) : QString();
}

LineFields CaptureLine::fields() const
{
    return { subject(), reply(), sequence(), size(), headers(), payload() };
}

Capture::Capture( const QStringList& lines )
{
    lines_.reserve( lines.size() );
    for ( const auto& line : lines ) {
        lines_ << CaptureLine::read( line );
    }
}

Capture Capture::read( const QString& path )
{
    return Capture( readLines( path ) );
}

Capture Capture::read( const QStringList& paths )
{
    QStringList lines;
    for ( const auto& path : paths ) {
        lines << readLines( path );
    }
    return Capture( lines );
}

QList<LineFields> Capture::fields() const
{
    QList<LineFields> result;
    for ( const auto& line : lines_ ) {
        result << line.fields();
    }
    return result;
}

QList<CaptureLine> Capture::messages() const
{
    QList<CaptureLine> result;
    for ( const auto& line : lines_ ) {
        if ( line.isMessage() ) {
            result << line;
        }
    }
    return result;
}

QList<CaptureLine> Capture::messagesOn( const QString& subject ) const
{
    QList<CaptureLine> result;
    for ( const auto& line : lines_ ) {
        if ( line.isMessage() && line.subject() == subject ) {
            result << line;
        }
    }
    return result;
}

QList<LineFields> Capture::messageFields() const
{
    QList<LineFields> result;
    for ( const auto& line : lines_ ) {
        if ( line.isMessage() ) {
            result << line.fields();
        }
    }
    return result;
}

QStringList Capture::payloadsOn( const QString& subject ) const
{
    QStringList result;
    for ( const auto& line : messagesOn( subject ) ) {
        result << line.payload();
    }
    return result;
}

QList<quint64> Capture::sequences() const
{
    QList<quint64> result;
    for ( const auto& line : lines_ ) {
        if ( line.isMessage() ) {
            INFO( "line: " << line.text().left( 200 ).toStdString() );
            REQUIRE( line.sequence() > 0 );
            result << line.sequence();
        }
    }
    return result;
}

QStringList Capture::markers() const
{
    QStringList result;
    for ( const auto& line : lines_ ) {
        if ( line.isMarker() ) {
            result << line.markerText();
        }
    }
    return result;
}

bool Capture::hasMarker( const QString& text ) const
{
    return std::any_of( lines_.begin(), lines_.end(), [ &text ]( const CaptureLine& line ) {
        return line.isMarker() && line.markerText() == text;
    } );
}

bool Capture::hasMarkerStartingWith( const QString& prefix ) const
{
    return std::any_of( lines_.begin(), lines_.end(), [ &prefix ]( const CaptureLine& line ) {
        return line.isMarker() && line.markerText().startsWith( prefix );
    } );
}

QList<CaptureLine> Capture::unreadable() const
{
    QList<CaptureLine> result;
    for ( const auto& line : lines_ ) {
        if ( line.kind() == CaptureLine::Kind::Unreadable ) {
            result << line;
        }
    }
    return result;
}

Drops Capture::drops() const
{
    // The text of a dropped= marker (natssession.cpp).
    static const QRegularExpression dropped(
        QStringLiteral( "^dropped=(\\d+) \\((slow consumer|queue full)\\)$" ) );
    Drops d;
    for ( const auto& line : lines_ ) {
        if ( line.isMessage() ) {
            continue;
        }
        const auto match = dropped.match( line.markerText() );
        if ( match.hasMatch() ) {
            const auto count = match.captured( 1 ).toLongLong();
            REQUIRE( count > 0 );
            ++d.markers;
            ( match.captured( 2 ) == QLatin1String( "slow consumer" ) ? d.slowConsumer
                                                                       : d.queueFull )
                += count;
        }
        else if ( line.text().contains( QLatin1String( "dropped=" ) ) ) {
            ++d.unknown;
        }
    }
    return d;
}

} // namespace nats_test

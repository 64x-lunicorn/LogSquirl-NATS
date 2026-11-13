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
 * @file capture.h
 * @brief The line reader of the tests: a capture's file read line by line
 *        with the Log Format (logformat.h), as the Table View reads it.
 *
 * The one place the tests know the line format (lineformat.h).  A line is
 * a message line, a marker line (the subject $NATS-MONITOR) or unreadable
 * (the Log Format does not match it); its columns are fields, and the body
 * is split into the header block and the payload as the formatter writes
 * them.  Tests compare fields, never substrings of a line:
 *
 *     const auto capture = nats_test::Capture::read( session.filePath() );
 *     REQUIRE( capture.first().markerText() == "connected to " + url + ", subscribed to x" );
 *     REQUIRE( capture.messageFields()
 *              == QList<nats_test::LineFields>{ { "x", "-", 0, 3, {}, "one" } } );
 */

#pragma once

#include <catch2/catch.hpp>

#include <QList>
#include <QString>
#include <QStringList>

#include <tuple>

namespace nats_test {

/// The fields of a line but its timestamp, to compare a line with.
struct LineFields {
    QString subject;
    QString reply;        ///< As written: "-" for none.
    quint64 sequence = 0; ///< The stream sequence of a replayed message; 0 for a live one.
    qint64 size = 0;      ///< The payload's size in bytes, as written.
    QString headers;      ///< The header block as written, without its brackets; empty for none.
    QString payload;      ///< The payload as written (escaped, or hex:…).

    bool operator==( const LineFields& other ) const
    {
        return std::tie( subject, reply, sequence, size, headers, payload )
               == std::tie( other.subject, other.reply, other.sequence, other.size, other.headers,
                            other.payload );
    }

    bool operator!=( const LineFields& other ) const
    {
        return !( *this == other );
    }
};

/// The fields of the marker line with the text @p text.
LineFields markerFields( const QString& text );

/// The fields of the line of a live message on @p subject with @p payload,
/// text that the formatter writes as it is (no escapes), and no reply
/// subject or headers.
LineFields liveFields( const QString& subject, const QString& payload );

/// One line of a capture's file, read with the Log Format.
class CaptureLine {
public:
    enum class Kind {
        Message,    ///< A NATS message.
        Marker,     ///< A line of the plugin itself: the subject $NATS-MONITOR.
        Unreadable, ///< The Log Format does not match it, or its header block is not closed.
    };

    /// @p text, a line without its terminator, read with the Log Format.
    static CaptureLine read( const QString& text );

    Kind kind() const
    {
        return kind_;
    }

    bool isMessage() const
    {
        return kind_ == Kind::Message;
    }

    bool isMarker() const
    {
        return kind_ == Kind::Marker;
    }

    /// The line as it is in the file.
    const QString& text() const
    {
        return text_;
    }

    // The columns, empty (0 for the numbers) for an unreadable line.
    QString timestamp() const;
    QString subject() const;
    QString reply() const;
    quint64 sequence() const;
    qint64 size() const;
    QString body() const;
    QString headers() const;
    QString payload() const;

    /// The text of a marker line, the payload; empty for any other line.
    QString markerText() const;

    /// The fields but the timestamp.
    LineFields fields() const;

private:
    /// Where in text_ a field is.
    struct Span {
        int start = 0;
        int length = 0;
    };

    QString span( Span s ) const;

    QString text_;
    Kind kind_ = Kind::Unreadable;
    Span timestamp_, subject_, reply_, body_, headers_, payload_;
    quint64 sequence_ = 0;
    qint64 size_ = 0;
};

/// What the dropped= markers of a capture count.
struct Drops {
    qint64 slowConsumer = 0; ///< The sum of dropped= of the (slow consumer) markers.
    qint64 queueFull = 0;    ///< The sum of dropped= of the (queue full) markers.
    int markers = 0;         ///< dropped= markers.
    int unknown = 0;         ///< Markers or unreadable lines that say dropped= in another shape.

    qint64 total() const
    {
        return slowConsumer + queueFull;
    }
};

/// The lines of a capture, read with CaptureLine::read().
class Capture {
public:
    Capture() = default;

    /// @p lines, without their terminators.
    explicit Capture( const QStringList& lines );

    /// The whole lines of the file at @p path (readLines() in sessiontest.h);
    /// empty if it cannot be read.
    static Capture read( const QString& path );

    /// The whole lines of the files @p paths, one after the other: a capture
    /// across its rotated files.
    static Capture read( const QStringList& paths );

    const QList<CaptureLine>& lines() const
    {
        return lines_;
    }

    int size() const
    {
        return static_cast<int>( lines_.size() );
    }

    bool isEmpty() const
    {
        return lines_.isEmpty();
    }

    const CaptureLine& at( int i ) const
    {
        return lines_.at( i );
    }

    const CaptureLine& first() const
    {
        return lines_.first();
    }

    const CaptureLine& last() const
    {
        return lines_.last();
    }

    /// The fields of every line, in order.
    QList<LineFields> fields() const;

    /// The message lines, in order.
    QList<CaptureLine> messages() const;

    /// The message lines on @p subject, in order.
    QList<CaptureLine> messagesOn( const QString& subject ) const;

    /// The fields of the message lines, in order.
    QList<LineFields> messageFields() const;

    /// The payloads of the message lines on @p subject, in order.
    QStringList payloadsOn( const QString& subject ) const;

    /// The stream sequences of the message lines, in order; fails the test
    /// if a message line has none (a live one).
    QList<quint64> sequences() const;

    /// The texts of the marker lines, in order.
    QStringList markers() const;

    /// Whether a marker line's text is @p text.
    bool hasMarker( const QString& text ) const;

    /// Whether a marker line's text starts with @p prefix.
    bool hasMarkerStartingWith( const QString& prefix ) const;

    /// The unreadable lines, in order.
    QList<CaptureLine> unreadable() const;

    /// What the dropped= markers count; fails the test on a dropped=0.
    Drops drops() const;

private:
    QList<CaptureLine> lines_;
};

} // namespace nats_test

namespace Catch {
template <>
struct StringMaker<nats_test::LineFields> {
    static std::string convert( const nats_test::LineFields& f )
    {
        return QStringLiteral( "{ \"%1\" \"%2\" #%3 %4 [%5] \"%6\" }" )
            .arg( f.subject, f.reply )
            .arg( f.sequence )
            .arg( f.size )
            .arg( f.headers, f.payload.left( 200 ) )
            .toStdString();
    }
};

template <>
struct StringMaker<nats_test::CaptureLine> {
    static std::string convert( const nats_test::CaptureLine& line )
    {
        return '"' + line.text().left( 200 ).toStdString() + '"';
    }
};
} // namespace Catch

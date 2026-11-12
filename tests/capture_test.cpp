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
 * @file capture_test.cpp
 * @brief BDD tests of the tests' line reader (capture.h).
 *
 * The lines come from the real formatter, so the reader, the formatter and
 * the Log Format cannot drift apart; the fields expected are written out by
 * hand.
 */

#include <catch2/catch.hpp>

#include "capture.h"
#include "lineformat.h"

using nats_monitor::formatLine;
using nats_monitor::formatMarker;
using nats_monitor::Message;
using nats_test::Capture;
using nats_test::CaptureLine;
using nats_test::LineFields;

namespace {

/// 2026-11-11 10:00:00.123456 UTC, in microseconds since the epoch.
constexpr qint64 kReceived = 1794391200123456LL;

Message message( const QByteArray& subject, const QByteArray& payload )
{
    Message m;
    m.receivedUs = kReceived;
    m.subject = subject;
    m.payload = payload;
    return m;
}

CaptureLine read( const QByteArray& line )
{
    return CaptureLine::read( QString::fromUtf8( line ) );
}

} // namespace

SCENARIO( "the line reader reads a message line into its fields", "[capture]" )
{
    GIVEN( "a live message with a reply subject, headers and a payload" )
    {
        auto m = message( "orders.eu.created", R"({"id":1,"total":9.5})" );
        m.reply = "_INBOX.ab12";
        m.headers = { { "Trace", "abc" }, { "Nats-Msg-Id", "42" } };
        const auto line = read( formatLine( m ) );

        THEN( "it is a message line, every column a field, the body split into headers and "
              "payload" )
        {
            REQUIRE( line.kind() == CaptureLine::Kind::Message );
            REQUIRE( line.isMessage() );
            REQUIRE_FALSE( line.isMarker() );
            REQUIRE( line.timestamp() == "2026-11-11 10:00:00.123456Z" );
            REQUIRE( line.fields()
                     == LineFields{ "orders.eu.created", "_INBOX.ab12", 0, 20,
                                    "Trace=abc; Nats-Msg-Id=42", R"({"id":1,"total":9.5})" } );
            REQUIRE( line.body() == R"([Trace=abc; Nats-Msg-Id=42] {"id":1,"total":9.5})" );
            REQUIRE( line.markerText().isEmpty() );
        }
    }

    GIVEN( "a message without a reply subject, headers or a payload" )
    {
        THEN( "the reply is a dash, the headers and the payload are empty" )
        {
            REQUIRE( read( formatLine( message( "ping", "" ) ) ).fields()
                     == LineFields{ "ping", "-", 0, 0, {}, {} } );
        }
    }

    GIVEN( "headers with an escaped ] and no payload" )
    {
        auto m = message( "h", "" );
        m.headers = { { "B", "x]y" } };

        THEN( "the header block ends at the bracket that is not escaped" )
        {
            REQUIRE( read( formatLine( m ) ).fields()
                     == LineFields{ "h", "-", 0, 0, R"(B=x\]y)", {} } );
        }
    }

    GIVEN( "a payload starting with [ and one with line breaks" )
    {
        THEN( "the payload is as written, escaped, and there are no headers" )
        {
            REQUIRE( read( formatLine( message( "t", "[1] up" ) ) ).fields()
                     == LineFields{ "t", "-", 0, 6, {}, R"(\x5B1] up)" } );
            REQUIRE( read( formatLine( message( "t", "a\nb" ) ) ).fields()
                     == LineFields{ "t", "-", 0, 3, {}, R"(a\nb)" } );
        }
    }

    GIVEN( "a message replayed from a stream" )
    {
        auto m = message( "orders.eu", "3" );
        m.streamSequence = 1042;

        THEN( "its stream sequence is a field" )
        {
            const auto line = read( formatLine( m ) );
            REQUIRE( line.sequence() == 1042 );
            REQUIRE( line.fields() == LineFields{ "orders.eu", "-", 1042, 1, {}, "3" } );
        }
    }
}

SCENARIO( "the line reader tells a marker line from a message line", "[capture]" )
{
    GIVEN( "a marker line" )
    {
        const auto line = read( formatMarker( kReceived, "connected to nats://127.0.0.1:4222" ) );

        THEN( "it is a marker, its text the payload" )
        {
            REQUIRE( line.kind() == CaptureLine::Kind::Marker );
            REQUIRE( line.isMarker() );
            REQUIRE_FALSE( line.isMessage() );
            REQUIRE( line.markerText() == "connected to nats://127.0.0.1:4222" );
            REQUIRE( line.subject() == "$NATS-MONITOR" );
            REQUIRE( line.size() == 34 );
            REQUIRE( line.fields()
                     == nats_test::markerFields( "connected to nats://127.0.0.1:4222" ) );
        }
    }

    GIVEN( "a live message line" )
    {
        THEN( "liveFields() makes the fields to compare it with" )
        {
            REQUIRE( read( formatLine( message( "a.b", "Grüße" ) ) ).fields()
                     == nats_test::liveFields( "a.b", "Grüße" ) );
            REQUIRE( nats_test::liveFields( "a.b", "Grüße" )
                     == LineFields{ "a.b", "-", 0, 7, {}, "Grüße" } );
        }
    }
}

SCENARIO( "the line reader reads no line the formatter does not write", "[capture]" )
{
    GIVEN( "lines the Log Format does not match, or with a header block not closed" )
    {
        const auto text = GENERATE(
            as<QString>{}, "", "garbage", "2026-11-11 10:00:00.123 orders - 20 x",
            "2026-11-11 10:00:00.123456Z orders - twenty x",
            "2026-11-11 10:00:00.123456Z h - 0 [a=b", "2026-11-11 10:00:00.123456Z h - 1 [a=b]x" );
        const auto line = CaptureLine::read( text );

        THEN( "it is unreadable, with no fields, and kept as it is" )
        {
            INFO( "line: " << text.toStdString() );
            REQUIRE( line.kind() == CaptureLine::Kind::Unreadable );
            REQUIRE_FALSE( line.isMessage() );
            REQUIRE_FALSE( line.isMarker() );
            REQUIRE( line.fields() == LineFields{} );
            REQUIRE( line.timestamp().isEmpty() );
            REQUIRE( line.markerText().isEmpty() );
            REQUIRE( line.text() == text );
        }
    }
}

SCENARIO( "a capture's lines are read as messages, markers and drops", "[capture]" )
{
    GIVEN( "a capture: connected, two replayed messages on two subjects, two dropped markers, "
           "an unreadable line, stopped" )
    {
        auto a = message( "a", "1" );
        a.streamSequence = 7;
        auto b = message( "b", "22" );
        b.streamSequence = 9;
        const Capture capture( QStringList{
            QString::fromUtf8( formatMarker( kReceived, "connected to nats://x:1, replaying" ) ),
            QString::fromUtf8( formatLine( a ) ),
            QString::fromUtf8( formatMarker( kReceived, "dropped=3 (slow consumer)" ) ),
            QString::fromUtf8( formatLine( b ) ),
            QString::fromUtf8( formatMarker( kReceived, "dropped=5 (queue full)" ) ),
            "dropped=1 not a line", QString::fromUtf8( formatMarker( kReceived, "stopped" ) ) } );

        THEN( "each line is there, in order" )
        {
            REQUIRE( capture.size() == 7 );
            REQUIRE( capture.first().markerText() == "connected to nats://x:1, replaying" );
            REQUIRE( capture.last().markerText() == "stopped" );
        }

        THEN( "the messages, their fields, payloads and sequences are read in order" )
        {
            REQUIRE( capture.fields().size() == 7 );
            REQUIRE( capture.fields().at( 6 ) == nats_test::markerFields( "stopped" ) );
            REQUIRE( capture.messages().size() == 2 );
            REQUIRE(
                capture.messageFields()
                == QList<LineFields>{ { "a", "-", 7, 1, {}, "1" }, { "b", "-", 9, 2, {}, "22" } } );
            REQUIRE( capture.messagesOn( "b" ).size() == 1 );
            REQUIRE( capture.payloadsOn( "a" ) == QStringList{ "1" } );
            REQUIRE( capture.sequences() == QList<quint64>{ 7, 9 } );
        }

        THEN( "the markers are read by their text" )
        {
            REQUIRE( capture.markers()
                     == QStringList{ "connected to nats://x:1, replaying",
                                     "dropped=3 (slow consumer)", "dropped=5 (queue full)",
                                     "stopped" } );
            REQUIRE( capture.hasMarker( "stopped" ) );
            REQUIRE_FALSE( capture.hasMarker( "stop" ) );
            REQUIRE( capture.hasMarkerStartingWith( "connected to " ) );
            REQUIRE_FALSE( capture.hasMarkerStartingWith( "replaying" ) );
        }

        THEN( "the dropped= markers are counted by kind, the unreadable line that says "
              "dropped= as unknown" )
        {
            const auto drops = capture.drops();
            REQUIRE( drops.slowConsumer == 3 );
            REQUIRE( drops.queueFull == 5 );
            REQUIRE( drops.total() == 8 );
            REQUIRE( drops.markers == 2 );
            REQUIRE( drops.unknown == 1 );
            REQUIRE( capture.unreadable().size() == 1 );
            REQUIRE( capture.unreadable().first().text() == "dropped=1 not a line" );
        }
    }
}

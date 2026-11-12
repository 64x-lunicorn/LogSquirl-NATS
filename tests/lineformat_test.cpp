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
 * @file lineformat_test.cpp
 * @brief BDD tests for the line format: one NATS message, one line.
 *
 * The formatter is a pure function, so the expected lines are written out
 * by hand from the format in issue #2.
 */

#include <catch2/catch.hpp>

#include "lineformat.h"

using nats_monitor::formatLine;
using nats_monitor::formatMarker;
using nats_monitor::FormatOptions;
using nats_monitor::Message;

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

} // namespace

SCENARIO( "a message becomes one line", "[lineformat]" )
{
    GIVEN( "a UTF-8 JSON message with a reply subject" )
    {
        auto m = message( "orders.eu.created", R"({"id":1,"total":9.5})" );
        m.reply = "_INBOX.ab12";

        THEN( "the line holds the UTC time in microseconds, subject, reply, size and payload" )
        {
            REQUIRE( formatLine( m )
                     == R"(2026-11-11 10:00:00.123456Z orders.eu.created )"
                        R"(_INBOX.ab12 20 {"id":1,"total":9.5})" );
        }
    }

    GIVEN( "a message without a reply subject" )
    {
        const auto m = message( "sensors.t1", "21.5" );

        THEN( "the reply field is a dash" )
        {
            REQUIRE( formatLine( m ) == "2026-11-11 10:00:00.123456Z sensors.t1 - 4 21.5" );
        }
    }

    GIVEN( "a time with fewer microseconds than six digits" )
    {
        auto m = message( "a", "x" );
        m.receivedUs = 1794391200000042LL;

        THEN( "the microseconds are padded with zeros" )
        {
            REQUIRE( formatLine( m ) == "2026-11-11 10:00:00.000042Z a - 1 x" );
        }
    }

    GIVEN( "non-ASCII UTF-8 text" )
    {
        const auto m = message( "greet", "Grüße, 世界 🐿" );

        THEN( "it stays as it is, and the size counts bytes" )
        {
            REQUIRE( formatLine( m ) == "2026-11-11 10:00:00.123456Z greet - 20 Grüße, 世界 🐿" );
        }
    }

    GIVEN( "text with embedded line breaks and backslashes" )
    {
        const auto m = message( "log", "a\nb\r\nc\\d" );

        THEN( "they are escaped, so the message stays one line" )
        {
            REQUIRE( formatLine( m ) == R"(2026-11-11 10:00:00.123456Z log - 8 a\nb\r\nc\\d)" );
        }
    }

    GIVEN( "text with a tab, a NUL and an escape character" )
    {
        const auto m = message( "ctl", QByteArray( "a\tb\0c\x1b", 6 ) );

        THEN( "the tab stays, the other control characters become \\xNN escapes" )
        {
            REQUIRE( formatLine( m ) == "2026-11-11 10:00:00.123456Z ctl - 6 a\tb\\x00c\\x1B" );
        }
    }

    GIVEN( "text that starts with [" )
    {
        const auto m = message( "t", "[1] up" );

        THEN( "the [ is a \\x5B escape, so it is not taken for headers" )
        {
            REQUIRE( formatLine( m ) == R"(2026-11-11 10:00:00.123456Z t - 6 \x5B1] up)" );
        }
    }

    GIVEN( "text that starts with hex:" )
    {
        const auto m = message( "t", "hex:00" );

        THEN( "the h is a \\x68 escape, so it is not taken for a binary payload" )
        {
            REQUIRE( formatLine( m ) == R"(2026-11-11 10:00:00.123456Z t - 6 \x68ex:00)" );
        }
    }

    GIVEN( "text with [ and hex: further on" )
    {
        const auto m = message( "t", "a [b] hex:" );

        THEN( "it stays as it is" )
        {
            REQUIRE( formatLine( m ) == "2026-11-11 10:00:00.123456Z t - 10 a [b] hex:" );
        }
    }

    GIVEN( "an empty payload" )
    {
        const auto m = message( "ping", "" );

        THEN( "the line ends after the size" )
        {
            REQUIRE( formatLine( m ) == "2026-11-11 10:00:00.123456Z ping - 0" );
        }
    }
}

SCENARIO( "headers go in brackets before the payload", "[lineformat]" )
{
    GIVEN( "a message with two headers" )
    {
        auto m = message( "sensors.t1", "21.5" );
        m.headers = { { "Nats-Msg-Id", "42" }, { "Trace", "abc" } };

        THEN( "they are written as [k=v; k=v]" )
        {
            REQUIRE( formatLine( m )
                     == "2026-11-11 10:00:00.123456Z sensors.t1 - 4 [Nats-Msg-Id=42; Trace=abc] "
                        "21.5" );
        }
    }

    GIVEN( "header values with ; ] \\ and line breaks" )
    {
        auto m = message( "h", "x" );
        m.headers = { { "A", "1;2" }, { "B", "x]y" }, { "C", "back\\slash" }, { "D", "l1\r\nl2" } };

        THEN( "they are escaped with a backslash" )
        {
            REQUIRE( formatLine( m )
                     == R"(2026-11-11 10:00:00.123456Z h - 1 [A=1\;2; B=x\]y; C=back\\slash; )"
                        R"(D=l1\r\nl2] x)" );
        }
    }

    GIVEN( "a header key with =" )
    {
        auto m = message( "h", "x" );
        m.headers = { { "a=b", "c=d" } };

        THEN( "the = in the key is escaped, so the first unescaped = ends the key" )
        {
            REQUIRE( formatLine( m ) == R"(2026-11-11 10:00:00.123456Z h - 1 [a\=b=c=d] x)" );
        }
    }

    GIVEN( "a message with headers and no payload" )
    {
        auto m = message( "h", "" );
        m.headers = { { "Status", "100" } };

        THEN( "the line ends after the headers" )
        {
            REQUIRE( formatLine( m ) == "2026-11-11 10:00:00.123456Z h - 0 [Status=100]" );
        }
    }
}

SCENARIO( "a payload that is not UTF-8 is written as hex", "[lineformat]" )
{
    GIVEN( "a binary payload" )
    {
        const auto m = message( "bin", QByteArray( "\x00\x01\xfe\xff", 4 ) );

        THEN( "it is written as hex: and lower-case hex digits" )
        {
            REQUIRE( formatLine( m ) == "2026-11-11 10:00:00.123456Z bin - 4 hex:0001feff" );
        }
    }

    GIVEN( "a binary payload longer than the cut length" )
    {
        const auto m = message( "bin", QByteArray( 1000, '\xff' ) );

        THEN( "only the first 256 bytes are written by default; the size is the real one" )
        {
            REQUIRE( formatLine( m )
                     == "2026-11-11 10:00:00.123456Z bin - 1000 hex:" + QByteArray( 512, 'f' ) );
        }

        THEN( "the cut length can be set" )
        {
            FormatOptions options;
            options.maxBinaryBytes = 3;
            REQUIRE( formatLine( m, options )
                     == "2026-11-11 10:00:00.123456Z bin - 1000 hex:ffffff" );
        }
    }

    GIVEN( "text cut inside a UTF-8 character" )
    {
        const auto m = message( "bad", QByteArray( "ab\xc3", 3 ) );

        THEN( "it is not valid UTF-8, so it is hex" )
        {
            REQUIRE( formatLine( m ) == "2026-11-11 10:00:00.123456Z bad - 3 hex:6162c3" );
        }
    }
}

SCENARIO( "a very large text payload stays whole on one line", "[lineformat]" )
{
    GIVEN( "a 4 MiB text payload with line breaks" )
    {
        const QByteArray chunk = "0123456789abcde\n";                            // 16 bytes
        const auto m = message( "big", chunk.repeated( 4 * 1024 * 1024 / 16 ) ); // 4 MiB

        THEN( "nothing is cut, and the line breaks are escaped" )
        {
            const auto expected = "2026-11-11 10:00:00.123456Z big - 4194304 "
                                  + QByteArray( "0123456789abcde\\n" ).repeated( 262144 );
            const auto line = formatLine( m );
            REQUIRE( line.size() == expected.size() );
            REQUIRE( line == expected );
        }
    }
}

SCENARIO( "a marker line has the same format", "[lineformat]" )
{
    GIVEN( "a connection event" )
    {
        THEN( "it is written with the subject $NATS-MONITOR" )
        {
            REQUIRE( formatMarker( kReceived, "disconnected" )
                     == "2026-11-11 10:00:00.123456Z $NATS-MONITOR - 12 disconnected" );
        }
    }
}

SCENARIO( "a message replayed from a JetStream stream carries its stream sequence",
          "[lineformat][replay]" )
{
    GIVEN( "a replayed message with its stored time and stream sequence 42" )
    {
        auto m = message( "orders.eu.created", R"({"id":1})" );
        m.streamSequence = 42;

        THEN( "the sequence follows the reply field as #42, before the size" )
        {
            REQUIRE( formatLine( m )
                     == R"(2026-11-11 10:00:00.123456Z orders.eu.created - #42 8 {"id":1})" );
        }
    }

    GIVEN( "a replayed message with headers and a payload starting with #" )
    {
        auto m = message( "s", "#1 up" );
        m.streamSequence = 18446744073709551615ULL; // the largest a stream has
        m.headers = { { "Nats-Msg-Id", "7" } };

        THEN( "headers and payload follow the size as in a live line" )
        {
            REQUIRE( formatLine( m )
                     == "2026-11-11 10:00:00.123456Z s - #18446744073709551615 5 "
                        "[Nats-Msg-Id=7] #1 up" );
        }
    }

    GIVEN( "a live message: no stream sequence" )
    {
        THEN( "there is no # field" )
        {
            REQUIRE( formatLine( message( "s", "#1" ) ) == "2026-11-11 10:00:00.123456Z s - 2 #1" );
        }
    }
}

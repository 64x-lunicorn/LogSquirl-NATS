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
 * @file replay_test.cpp
 * @brief BDD tests for replaying a JetStream stream, against a real
 *        nats-server with JetStream.
 *
 * The main seam, as natssession_test.cpp: a test starts a server with -js,
 * fills a stream with nats.c, starts a replaying session and reads the
 * lines of its file.  Only what the file, the session's signals and the
 * server show is asserted.  Where a replay starts and what it skips is
 * planned without a server (replayplan_test.cpp); here each deliver policy
 * is replayed once, to see nats.c called as the plan says.
 */

// First: on Windows nats.h includes winsock2.h, which must come before any
// windows.h.
#include <nats.h>

#include <catch2/catch.hpp>

#include "capture.h"
#include "lineformat.h"
#include "natsserver.h"
#include "natssession.h"
#include "sessiontest.h"
#include "streams.h"
#include "tcpproxy.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QProcess>
#include <QProcessEnvironment>

#include <chrono>
#include <iostream>
#include <thread>

using nats_monitor::NatsSession;
using nats_monitor::ReplayStart;
using nats_test::Capture;
using nats_test::CaptureLine;
using nats_test::JetStream;
using nats_test::LineFields;
using nats_test::NatsServer;
using nats_test::NatsServerOptions;
using nats_test::Publisher;
using nats_test::readLines;
using nats_test::replayConfig;
using nats_test::sessionConfig;
using nats_test::sessionTempRoot;
using nats_test::TcpProxy;
using nats_test::waitFor;
using nats_test::withJetStream;

namespace {

/// The sequences @p first to @p last.
QList<quint64> range( quint64 first, quint64 last )
{
    QList<quint64> result;
    for ( auto s = first; s <= last; ++s ) {
        result << s;
    }
    return result;
}

/// Start @p session and wait until it ended on its own, the replay complete.
void replayToTheEnd( NatsSession& session, int timeoutMs = 10000 )
{
    int completed = 0;
    QObject::connect( &session, &NatsSession::completed, [ &completed ] { ++completed; } );
    REQUIRE( session.start() );
    REQUIRE( waitFor( [ & ] { return completed == 1; }, timeoutMs ) );
    REQUIRE_FALSE( session.isRunning() );
}

/// Start @p session, a replay that follows live, and wait until it caught up.
void startAndCatchUp( NatsSession& session )
{
    REQUIRE( session.start() );
    REQUIRE( waitFor(
        [ & ] {
            return Capture::read( session.filePath() ).hasMarker( "caught up, following live" );
        },
        10000 ) );
}

} // namespace

SCENARIO( "a stream is replayed into the session's file", "[server][replay]" )
{
    SKIP_WITHOUT_NATS_SERVER();

    GIVEN( "a stream with three messages, one with headers" )
    {
        NatsServer server( withJetStream() );
        Publisher publisher( server.url() );
        JetStream js( publisher );
        js.addStream( "ORDERS", { "orders.>" } );
        js.publish( "orders.eu.created", R"({"id":1})" );
        js.publish( "orders.us.created", R"({"id":2})", { { "Trace", "abc" } } );
        js.publish( "orders.eu.cancelled", R"({"id":1})" );

        WHEN( "a session replays it from its start" )
        {
            NatsSession session( replayConfig( server.url(), "ORDERS" ), sessionTempRoot() );
            replayToTheEnd( session );
            const auto capture = Capture::read( session.filePath() );

            THEN( "each message is a line with its stored time and its stream sequence, in order" )
            {
                REQUIRE( capture.messageFields()
                         == QList<LineFields>{
                             { "orders.eu.created", "-", 1, 8, {}, R"({"id":1})" },
                             { "orders.us.created", "-", 2, 8, "Trace=abc", R"({"id":2})" },
                             { "orders.eu.cancelled", "-", 3, 8, {}, R"({"id":1})" } } );
                REQUIRE( capture.unreadable().isEmpty() );
                const auto messages = capture.messages();
                for ( int i = 0; i < messages.size(); ++i ) {
                    REQUIRE( messages.at( i ).timestamp()
                             == QString::fromLatin1( nats_monitor::formatTimestamp(
                                 js.storedUs( "ORDERS", static_cast<quint64>( i + 1 ) ) ) ) );
                }
            }

            THEN( "the first line says what is replayed, the last that the replay is complete" )
            {
                REQUIRE( capture.first().markerText()
                         == QStringLiteral( "connected to %1, replaying stream ORDERS" )
                                .arg( server.url() ) );
                REQUIRE( capture.last().markerText() == "replay complete" );
            }

            THEN( "the stream still holds its three messages" )
            {
                REQUIRE( js.messageCount( "ORDERS" ) == 3 );
            }
        }
    }
}

SCENARIO( "a replay starts where it is told to", "[server][replay]" )
{
    SKIP_WITHOUT_NATS_SERVER();

    GIVEN( "a stream of ten messages, the odd ones on orders.eu, the even ones on orders.us" )
    {
        NatsServer server( withJetStream() );
        Publisher publisher( server.url() );
        JetStream js( publisher );
        js.addStream( "ORDERS", { "orders.>" } );
        for ( int i = 1; i <= 10; ++i ) {
            if ( i == 6 ) {
                nats_test::settle( 20 ); // 6 is stored later than 5
            }
            js.publish( i % 2 ? "orders.eu" : "orders.us", QByteArray::number( i ) );
        }
        auto config = replayConfig( server.url(), "ORDERS" );

        WHEN( "it replays the last 3 messages" )
        {
            config.replay.start = ReplayStart::Last;
            config.replay.lastMessages = 3;
            NatsSession session( config, sessionTempRoot() );
            replayToTheEnd( session );

            THEN( "it writes 8, 9 and 10, and says so first" )
            {
                const auto capture = Capture::read( session.filePath() );
                REQUIRE( capture.sequences() == range( 8, 10 ) );
                REQUIRE(
                    capture.first().markerText().endsWith( "stream ORDERS, the last 3 messages" ) );
            }
        }

        WHEN( "it replays from sequence 7" )
        {
            config.replay.start = ReplayStart::Sequence;
            config.replay.sequence = 7;
            NatsSession session( config, sessionTempRoot() );
            replayToTheEnd( session );

            THEN( "it writes 7 to 10" )
            {
                const auto capture = Capture::read( session.filePath() );
                REQUIRE( capture.sequences() == range( 7, 10 ) );
                REQUIRE(
                    capture.first().markerText().endsWith( "stream ORDERS, from sequence 7" ) );
            }
        }

        WHEN( "it replays from the time message 6 was stored" )
        {
            config.replay.start = ReplayStart::Time;
            config.replay.timeUs = js.storedUs( "ORDERS", 6 );
            NatsSession session( config, sessionTempRoot() );
            replayToTheEnd( session );

            THEN( "it writes 6 to 10, and names the time as the lines do" )
            {
                const auto capture = Capture::read( session.filePath() );
                REQUIRE( capture.sequences() == range( 6, 10 ) );
                REQUIRE( capture.first().markerText().endsWith(
                    ", from " + capture.messages().first().timestamp() ) );
            }
        }

        WHEN( "it replays only orders.us" )
        {
            config.replay.filterSubject = "orders.us";
            NatsSession session( config, sessionTempRoot() );
            replayToTheEnd( session );

            THEN( "it writes the even ones" )
            {
                const auto capture = Capture::read( session.filePath() );
                REQUIRE( capture.sequences() == QList<quint64>{ 2, 4, 6, 8, 10 } );
                REQUIRE(
                    capture.first().markerText().endsWith( "stream ORDERS, subject orders.us" ) );
            }
        }

        WHEN( "it replays the last 2 messages on orders.eu" )
        {
            config.replay.filterSubject = "orders.eu";
            config.replay.start = ReplayStart::Last;
            config.replay.lastMessages = 2;
            NatsSession session( config, sessionTempRoot() );
            replayToTheEnd( session );

            THEN( "it writes the last two odd ones, 7 and 9" )
            {
                REQUIRE( Capture::read( session.filePath() ).sequences()
                         == QList<quint64>{ 7, 9 } );
            }
        }

        WHEN( "it replays from sequence 11, after the last" )
        {
            config.replay.start = ReplayStart::Sequence;
            config.replay.sequence = 11;
            NatsSession session( config, sessionTempRoot() );
            replayToTheEnd( session );

            THEN( "it writes no message and is complete at once" )
            {
                const auto capture = Capture::read( session.filePath() );
                REQUIRE( capture.sequences().isEmpty() );
                REQUIRE( capture.last().markerText() == "replay complete" );
            }
        }

        WHEN( "it has been replayed in full" )
        {
            NatsSession session( config, sessionTempRoot() );
            replayToTheEnd( session );

            THEN( "the stream still holds its ten messages" )
            {
                REQUIRE( js.messageCount( "ORDERS" ) == 10 );
            }
        }
    }

    GIVEN( "an empty stream" )
    {
        NatsServer server( withJetStream() );
        Publisher publisher( server.url() );
        JetStream js( publisher );
        js.addStream( "EMPTY", { "empty.>" } );

        THEN( "its replay is complete at once, with no message" )
        {
            NatsSession session( replayConfig( server.url(), "EMPTY" ), sessionTempRoot() );
            replayToTheEnd( session );
            const auto capture = Capture::read( session.filePath() );
            REQUIRE( capture.messages().isEmpty() );
            REQUIRE( capture.unreadable().isEmpty() );
        }
    }
}

SCENARIO( "a replay from a time early in its second starts at that time", "[server][replay]" )
{
    SKIP_WITHOUT_NATS_SERVER();

    // nats.c 3.14 writes a start time's fraction of a second without its
    // leading zeros: 12:00:00.025 goes to the server as 12:00:00.25, past
    // messages stored in that quarter of a second.
    GIVEN( "five messages from just after the start of a second, one of them (k) stored after "
           "an earlier one of the same second, before its first tenth is over" )
    {
        NatsServer server( withJetStream() );
        Publisher publisher( server.url() );
        JetStream js( publisher );
        // Until the clock is 2 to 12 ms into a second: sleeps to just
        // before, then spins, as a sleep of a few milliseconds may last far
        // longer (macOS coalesces timers).
        const auto waitForNextSecond = [] {
            for ( ;; ) {
                const auto into = static_cast<int>( QDateTime::currentMSecsSinceEpoch() % 1000 );
                if ( into >= 2 && into < 12 ) {
                    return;
                }
                const auto until = ( 2 - into + 1000 ) % 1000;
                if ( until > 3 ) {
                    std::this_thread::sleep_for( std::chrono::milliseconds( until - 3 ) );
                }
                else {
                    std::this_thread::yield();
                }
            }
        };
        // Message k > 1 is the latest stored in the first tenth of the
        // second of message 1, and later than message k - 1.  However slow
        // the machine, the publishing is not timed; a stream where no such k
        // exists is tried again, in a new stream.
        QByteArray stream;
        int k = 0;
        qint64 kUs = 0;
        for ( int attempt = 1; attempt <= 10 && k == 0; ++attempt ) {
            stream = "ORDERS" + QByteArray::number( attempt );
            const auto subjects = "orders" + QByteArray::number( attempt );
            js.addStream( stream.constData(), { ( subjects + ".>" ).constData() } );
            waitForNextSecond();
            for ( int i = 1; i <= 5; ++i ) {
                js.publish( ( subjects + ".eu" ).constData(), QByteArray::number( i ) );
            }
            const auto firstSecond = js.storedUs( stream.constData(), 1 ) / 1000000;
            for ( int i = 5; i >= 2 && k == 0; --i ) {
                const auto us = js.storedUs( stream.constData(), i );
                if ( us / 1000000 == firstSecond && us % 1000000 < 100000
                     && us > js.storedUs( stream.constData(), i - 1 ) ) {
                    k = i;
                    kUs = us;
                }
            }
        }
        REQUIRE( k != 0 );

        WHEN( "it replays from the time message k was stored" )
        {
            auto config = replayConfig( server.url(), QString::fromLatin1( stream ) );
            config.replay.start = ReplayStart::Time;
            config.replay.timeUs = kUs;
            NatsSession session( config, sessionTempRoot() );
            replayToTheEnd( session );

            THEN( "it writes k to 5, none before" )
            {
                REQUIRE( Capture::read( session.filePath() ).sequences() == range( k, 5 ) );
            }
        }
    }
}

SCENARIO( "a replay may go on live once it has caught up", "[server][replay]" )
{
    SKIP_WITHOUT_NATS_SERVER();

    GIVEN( "a stream of three messages and a replay of it that follows live" )
    {
        NatsServer server( withJetStream() );
        Publisher publisher( server.url() );
        JetStream js( publisher );
        js.addStream( "ORDERS", { "orders.>" } );
        for ( int i = 1; i <= 3; ++i ) {
            js.publish( "orders.eu", QByteArray::number( i ) );
        }
        auto config = replayConfig( server.url(), "ORDERS" );
        config.replay.followLive = true;
        NatsSession session( config, sessionTempRoot() );
        int completed = 0;
        QObject::connect( &session, &NatsSession::completed, [ &completed ] { ++completed; } );
        REQUIRE( session.start() );
        const auto caughtUp = [ & ] {
            return Capture::read( session.filePath() ).hasMarker( "caught up, following live" );
        };
        REQUIRE( waitFor( caughtUp, 10000 ) );

        THEN( "it says so after the three, and still runs" )
        {
            const auto capture = Capture::read( session.filePath() );
            REQUIRE( capture.first().markerText().endsWith( "stream ORDERS, then live" ) );
            REQUIRE( capture.sequences() == range( 1, 3 ) );
            REQUIRE( capture.last().markerText() == "caught up, following live" );
            REQUIRE( session.isRunning() );
            REQUIRE( completed == 0 );
        }

        WHEN( "the stream stores two more, one published without JetStream" )
        {
            js.publish( "orders.us", "4" );
            publisher.publish( "orders.eu", "5" );

            THEN( "they are written after the marker, until stopped" )
            {
                REQUIRE( waitFor(
                    [ & ] { return Capture::read( session.filePath() ).sequences().size() == 5; },
                    5000 ) );
                session.stop();
                const auto capture = Capture::read( session.filePath() );
                REQUIRE( capture.sequences() == range( 1, 5 ) );
                REQUIRE( capture.at( capture.size() - 3 ).fields()
                         == LineFields{ "orders.us", "-", 4, 1, {}, "4" } );
                REQUIRE( capture.last().markerText() == "stopped" );
                REQUIRE( js.messageCount( "ORDERS" ) == 5 );
            }
        }
    }
}

SCENARIO( "a replay of a stream that does not exist fails, saying why", "[server][replay]" )
{
    SKIP_WITHOUT_NATS_SERVER();

    GIVEN( "a server with JetStream but no stream NOPE" )
    {
        NatsServer server( withJetStream() );
        NatsSession session( replayConfig( server.url(), "NOPE" ), sessionTempRoot() );
        int finished = 0;
        QObject::connect( &session, &NatsSession::finished, [ &finished ] { ++finished; } );

        THEN( "its replay ends on its own, with an error marker naming the stream" )
        {
            REQUIRE( session.start() );
            REQUIRE( waitFor( [ & ] { return finished == 1; }, 10000 ) );
            const auto capture = Capture::read( session.filePath() );
            REQUIRE( capture.hasMarkerStartingWith( "replaying stream NOPE failed: " ) );
        }
    }
}

SCENARIO( "a stream of a million messages is replayed whole, or stopped at any point",
          "[server][replay][large]" )
{
    SKIP_WITHOUT_NATS_SERVER();

    GIVEN( "a stream of 1,000,000 messages" )
    {
        constexpr int kCount = 1000000;
        NatsServer server( withJetStream() );
        Publisher publisher( server.url() );
        JetStream js( publisher );
        js.addStream( "BIG", { "big.>" } );
        js.publishMany( "BIG", "big.data", kCount );
        auto config = replayConfig( server.url(), "BIG" );
        config.maxFileBytes = 0; // one file to read

        /// Message line i (from 0) is the stream's message i + 1, payload i + 1.
        const auto requireInOrder = []( const QList<CaptureLine>& messages ) {
            for ( int i = 0; i < messages.size(); ++i ) {
                const auto n = QString::number( i + 1 );
                if ( messages.at( i ).fields()
                     != LineFields{ "big.data", "-", static_cast<quint64>( i + 1 ), n.size(), {},
                                    n } ) {
                    FAIL( "message line " << i << ": " << messages.at( i ).text().toStdString() );
                }
            }
        };

        WHEN( "it is replayed to its end" )
        {
            NatsSession session( config, sessionTempRoot() );
            replayToTheEnd( session, 120000 );
            const auto capture = Capture::read( session.filePath() );

            THEN( "every message is a line, in order, none dropped" )
            {
                const auto messages = capture.messages();
                REQUIRE( messages.size() == kCount );
                requireInOrder( messages );
                REQUIRE( capture.unreadable().isEmpty() );
                const auto drops = capture.drops();
                REQUIRE( drops.markers == 0 );
                REQUIRE( drops.unknown == 0 );
                REQUIRE( session.droppedCount() == 0 );
                REQUIRE( capture.last().markerText() == "replay complete" );
                REQUIRE( js.messageCount( "BIG" ) == kCount );
            }
        }

        // Stopped once more than @p lines messages arrived (0: at once,
        // before any): it stops at once, what it wrote until then in order,
        // the stream unchanged.
        const auto requireStopsAt = [ & ]( int lines ) {
            NatsSession session( config, sessionTempRoot() );
            REQUIRE( session.start() );
            if ( lines > 0 ) {
                // Counted, not read from the file: reading half a million
                // lines may take longer than the rest of the replay.
                REQUIRE( waitFor(
                    [ & ] { return session.receivedCount() > static_cast<quint64>( lines ); },
                    30000 ) );
            }
            QElapsedTimer timer;
            timer.start();
            session.stop();
            const auto stopMs = timer.elapsed();
            const auto capture = Capture::read( session.filePath() );
            REQUIRE( stopMs < 3000 );
            REQUIRE( capture.last().markerText() == "stopped" );
            const auto messages = capture.messages();
            REQUIRE( messages.size() < kCount );
            requireInOrder( messages );
            REQUIRE( capture.unreadable().isEmpty() );
            const auto drops = capture.drops();
            REQUIRE( drops.markers == 0 );
            REQUIRE( drops.unknown == 0 );
            REQUIRE( js.messageCount( "BIG" ) == kCount );
        };

        WHEN( "it is stopped right after it started" )
        {
            THEN( "it stops at once, what it wrote in order, the stream unchanged" )
            {
                requireStopsAt( 0 );
            }
        }

        WHEN( "it is stopped once the first lines are written" )
        {
            THEN( "it stops at once, what it wrote until then in order, the stream unchanged" )
            {
                requireStopsAt( 1000 );
            }
        }

        WHEN( "it is stopped half-way" )
        {
            THEN( "it stops at once, what it wrote until then in order, the stream unchanged" )
            {
                requireStopsAt( kCount / 2 );
            }
        }
    }
}

SCENARIO( "a replay whose writer cannot keep up is still written whole", "[server][replay]" )
{
    SKIP_WITHOUT_NATS_SERVER();

    GIVEN( "a stream of 20,000 messages and a replay whose queue holds one message" )
    {
        // Each message waits until the writer has written and flushed the
        // one before: the delivery thread is held up all the time, which
        // JetStream's flow control and heartbeats must survive.
        constexpr int kCount = 20000;
        NatsServer server( withJetStream() );
        Publisher publisher( server.url() );
        JetStream js( publisher );
        js.addStream( "BIG", { "big.>" } );
        js.publishMany( "BIG", "big.data", kCount );
        auto config = replayConfig( server.url(), "BIG" );
        config.queueLimit = 1;

        WHEN( "it is replayed to its end" )
        {
            NatsSession session( config, sessionTempRoot() );
            replayToTheEnd( session, 120000 );
            const auto capture = Capture::read( session.filePath() );

            THEN( "every message is a line, once and in order, none dropped" )
            {
                REQUIRE( capture.sequences() == range( 1, kCount ) );
                REQUIRE( capture.unreadable().isEmpty() );
                const auto drops = capture.drops();
                REQUIRE( drops.markers == 0 );
                REQUIRE( drops.unknown == 0 );
                REQUIRE( session.droppedCount() == 0 );
                REQUIRE( capture.last().markerText() == "replay complete" );
            }
        }

        WHEN( "it is stopped once the first lines are written" )
        {
            // nats.c holds what the writer has yet to take, up to the
            // pending limits: the stream keeps it, a stop does not wait
            // for it.
            NatsSession session( config, sessionTempRoot() );
            REQUIRE( session.start() );
            REQUIRE(
                waitFor( [ & ] { return readLines( session.filePath() ).size() > 100; }, 30000 ) );
            QElapsedTimer timer;
            timer.start();
            session.stop();
            const auto stopMs = timer.elapsed();
            const auto capture = Capture::read( session.filePath() );

            THEN( "it stops at once, what it wrote until then in order, none dropped" )
            {
                REQUIRE( stopMs < 3000 );
                REQUIRE( capture.last().markerText() == "stopped" );
                const auto written = capture.sequences();
                REQUIRE( written.size() < kCount );
                REQUIRE( written == range( 1, written.size() ) );
                REQUIRE( capture.unreadable().isEmpty() );
                const auto drops = capture.drops();
                REQUIRE( drops.markers == 0 );
                REQUIRE( drops.unknown == 0 );
                REQUIRE( session.droppedCount() == 0 );
                REQUIRE( js.messageCount( "BIG" ) == kCount );
            }
        }
    }
}

SCENARIO( "the streams to replay are listed from the server", "[server][replay]" )
{
    SKIP_WITHOUT_NATS_SERVER();

    GIVEN( "a server with the streams ORDERS and AUDIT" )
    {
        NatsServer server( withJetStream() );
        Publisher publisher( server.url() );
        JetStream js( publisher );
        js.addStream( "ORDERS", { "orders.>" } );
        js.addStream( "AUDIT", { "audit.>" } );
        const auto config = sessionConfig( server.url() );

        THEN( "both are listed, by name" )
        {
            QString error;
            REQUIRE( nats_monitor::listStreams( config, &error )
                     == QStringList{ "AUDIT", "ORDERS" } );
            REQUIRE( error.isEmpty() );
        }
    }

    GIVEN( "a server without JetStream" )
    {
        NatsServer server;
        const auto config = sessionConfig( server.url() );

        THEN( "no stream is listed, and the error says why" )
        {
            QString error;
            REQUIRE( nats_monitor::listStreams( config, &error ).isEmpty() );
            REQUIRE_FALSE( error.isEmpty() );
        }
    }

    GIVEN( "no server" )
    {
        const auto config = sessionConfig( "nats://127.0.0.1:1" );

        THEN( "no stream is listed, and the error says why" )
        {
            QString error;
            REQUIRE( nats_monitor::listStreams( config, &error ).isEmpty() );
            REQUIRE( error.contains( "127.0.0.1:1" ) );
        }
    }
}

SCENARIO( "a replay without a stream name does not start", "[replay]" )
{
    GIVEN( "a replay of the stream \" \"" )
    {
        THEN( "start() fails" )
        {
            NatsSession session( replayConfig( "nats://127.0.0.1:1", " " ), sessionTempRoot() );
            REQUIRE_FALSE( session.start() );
        }
    }
}

SCENARIO( "a replay leaves no consumer behind", "[server][replay][cleanup]" )
{
    SKIP_WITHOUT_NATS_SERVER();

    GIVEN( "a stream of three messages, with a durable consumer of its own" )
    {
        NatsServer server( withJetStream() );
        Publisher publisher( server.url() );
        JetStream js( publisher );
        js.addStream( "ORDERS", { "orders.>" } );
        js.addDurable( "ORDERS", "ORDERS_READER" );
        for ( int i = 1; i <= 3; ++i ) {
            js.publish( "orders.eu", QByteArray::number( i ) );
        }
        const auto before = js.consumers( "ORDERS" );
        REQUIRE( before == QStringList{ "ORDERS_READER" } );

        WHEN( "a replay that follows live runs" )
        {
            auto config = replayConfig( server.url(), "ORDERS" );
            config.replay.followLive = true;
            NatsSession session( config, sessionTempRoot() );
            startAndCatchUp( session );

            THEN( "the stream has one consumer more while it runs, and the same as before once "
                  "it is stopped" )
            {
                REQUIRE( js.consumers( "ORDERS" ).size() == before.size() + 1 );
                session.stop();
                REQUIRE( js.consumers( "ORDERS" ) == before );
            }
        }

        WHEN( "a replay ends by itself, complete" )
        {
            NatsSession session( replayConfig( server.url(), "ORDERS" ), sessionTempRoot() );
            replayToTheEnd( session );

            THEN( "the stream has the same consumers as before" )
            {
                REQUIRE( js.consumers( "ORDERS" ) == before );
            }
        }
    }
}

namespace {

/// The test, run in a process of its own, that the next scenario kills.
constexpr const char* kKilledReplay = "a replay runs in a process of its own until it is killed";

} // namespace

// Run by the next scenario only, as a child process: a replay of the stream
// LOGSQUIRL_NATS_REPLAY_STREAM on LOGSQUIRL_NATS_REPLAY_URL, with the
// consumer's inactive threshold LOGSQUIRL_NATS_REPLAY_INACTIVE_MS.  It says
// "caught up" on stdout and runs until it is killed.
TEST_CASE( "a replay runs in a process of its own until it is killed", "[.][child]" )
{
    const auto env = QProcessEnvironment::systemEnvironment();
    auto config = replayConfig( env.value( "LOGSQUIRL_NATS_REPLAY_URL" ),
                            env.value( "LOGSQUIRL_NATS_REPLAY_STREAM" ) );
    config.replay.followLive = true;
    config.replay.inactiveThresholdMs = env.value( "LOGSQUIRL_NATS_REPLAY_INACTIVE_MS" ).toInt();
    NatsSession session( config, sessionTempRoot() );
    startAndCatchUp( session );
    std::cout << "caught up" << std::endl;
    waitFor( [] { return false; }, 60000 );
}

SCENARIO( "the consumer of a replay that is killed disappears after its inactive threshold",
          "[server][replay][cleanup]" )
{
    SKIP_WITHOUT_NATS_SERVER();

    GIVEN( "a replay with an inactive threshold of 500 ms, in a process of its own" )
    {
        NatsServer server( withJetStream() );
        Publisher publisher( server.url() );
        JetStream js( publisher );
        js.addStream( "ORDERS", { "orders.>" } );
        js.addDurable( "ORDERS", "ORDERS_READER" );
        js.publish( "orders.eu", "1" );
        const auto before = js.consumers( "ORDERS" );

        QProcess child;
        auto env = QProcessEnvironment::systemEnvironment();
        env.insert( "LOGSQUIRL_NATS_REPLAY_URL", server.url() );
        env.insert( "LOGSQUIRL_NATS_REPLAY_STREAM", "ORDERS" );
        env.insert( "LOGSQUIRL_NATS_REPLAY_INACTIVE_MS", "500" );
        child.setProcessEnvironment( env );
        child.start( QCoreApplication::applicationFilePath(), { kKilledReplay } );
        QByteArray output;
        REQUIRE( waitFor(
            [ & ] {
                output += child.readAllStandardOutput();
                return output.contains( "caught up" );
            },
            10000 ) );
        REQUIRE( js.consumers( "ORDERS" ).size() == before.size() + 1 );

        WHEN( "the process is killed" )
        {
            child.kill();
            REQUIRE( child.waitForFinished( 5000 ) );

            THEN( "its consumer is gone soon after the threshold (the plugin's default is 5 s), "
                  "the stream's own consumer kept" )
            {
                QElapsedTimer timer;
                timer.start();
                REQUIRE( waitFor( [ & ] { return js.consumers( "ORDERS" ) == before; }, 3000 ) );
                INFO( "gone after " << timer.elapsed() << " ms" );
            }
        }
    }
}

SCENARIO( "a replay that loses its connection leaves no consumer behind",
          "[server][replay][cleanup]" )
{
    SKIP_WITHOUT_NATS_SERVER();

    GIVEN( "a replay that follows live through a connection that can be cut" )
    {
        NatsServer server( withJetStream() );
        Publisher publisher( server.url() );
        JetStream js( publisher );
        js.addStream( "ORDERS", { "orders.>" } );
        js.addDurable( "ORDERS", "ORDERS_READER" );
        js.publish( "orders.eu", "1" );
        const auto before = js.consumers( "ORDERS" );
        TcpProxy proxy( server.port() );
        auto config = replayConfig( proxy.url(), "ORDERS" );
        config.replay.followLive = true;
        config.replay.inactiveThresholdMs = 500;
        NatsSession session( config, sessionTempRoot() );
        startAndCatchUp( session );
        REQUIRE( js.consumers( "ORDERS" ).size() == before.size() + 1 );

        WHEN( "the connection is cut and comes back" )
        {
            proxy.cut();
            REQUIRE( waitFor(
                [ & ] {
                    return Capture::read( session.filePath() )
                        .hasMarkerStartingWith( "reconnected to " );
                },
                15000 ) );

            THEN( "the replay goes on, with one consumer more, and the stream has the same "
                  "consumers as before once it is stopped" )
            {
                js.publish( "orders.eu", "2" );
                REQUIRE( waitFor(
                    [ & ] { return Capture::read( session.filePath() ).sequences().contains( 2 ); },
                    20000 ) );
                REQUIRE( waitFor(
                    [ & ] { return js.consumers( "ORDERS" ).size() == before.size() + 1; } ) );
                session.stop();
                REQUIRE( waitFor( [ & ] { return js.consumers( "ORDERS" ) == before; } ) );
            }
        }

        WHEN( "the connection is cut for good and the replay is stopped while disconnected" )
        {
            proxy.refuse();
            proxy.cut();
            REQUIRE( waitFor(
                [ & ] { return Capture::read( session.filePath() ).hasMarker( "disconnected" ); },
                10000 ) );
            session.stop();

            THEN( "its consumer is gone after the inactive threshold, the stream's own kept" )
            {
                REQUIRE( waitFor( [ & ] { return js.consumers( "ORDERS" ) == before; }, 3000 ) );
            }
        }
    }
}

SCENARIO( "a stream that removes what its consumers took is refused or replayed with a warning",
          "[server][replay][retention]" )
{
    SKIP_WITHOUT_NATS_SERVER();

    NatsServer server( withJetStream() );
    Publisher publisher( server.url() );
    JetStream js( publisher );

    GIVEN( "a work queue stream of three jobs" )
    {
        js.addStream( "JOBS", { "jobs.>" }, js_WorkQueuePolicy );
        for ( int i = 1; i <= 3; ++i ) {
            js.publish( "jobs.new", QByteArray::number( i ) );
        }

        WHEN( "a session replays it" )
        {
            NatsSession session( replayConfig( server.url(), "JOBS" ), sessionTempRoot() );
            int finished = 0;
            QObject::connect( &session, &NatsSession::finished, [ &finished ] { ++finished; } );
            REQUIRE( session.start() );
            REQUIRE( waitFor( [ & ] { return finished == 1; }, 10000 ) );

            THEN( "it refuses, saying why, and the jobs are still there, with no consumer" )
            {
                const auto capture = Capture::read( session.filePath() );
                REQUIRE( capture.messages().isEmpty() );
                REQUIRE( capture.unreadable().isEmpty() );
                REQUIRE( capture.hasMarkerStartingWith(
                    "replaying stream JOBS failed: it is a work queue" ) );
                REQUIRE( js.messageCount( "JOBS" ) == 3 );
                REQUIRE( js.consumers( "JOBS" ).isEmpty() );
            }
        }
    }

    GIVEN( "an interest stream of three events, which its consumer EVENTS_READER has yet to read" )
    {
        js.addStream( "EVENTS", { "events.>" }, js_InterestPolicy );
        js.addDurable( "EVENTS", "EVENTS_READER" );
        for ( int i = 1; i <= 3; ++i ) {
            js.publish( "events.new", QByteArray::number( i ) );
        }

        WHEN( "a session replays it" )
        {
            NatsSession session( replayConfig( server.url(), "EVENTS" ), sessionTempRoot() );
            replayToTheEnd( session );
            const auto capture = Capture::read( session.filePath() );

            THEN( "it warns before the messages, then writes them" )
            {
                REQUIRE( capture.size() >= 2 );
                REQUIRE( capture.at( 1 ).markerText()
                             .startsWith( "warning: stream EVENTS has "
                                          "interest retention" ) );
                REQUIRE( capture.sequences() == range( 1, 3 ) );
            }

            THEN( "the stream still holds them for EVENTS_READER, its only consumer" )
            {
                REQUIRE( js.messageCount( "EVENTS" ) == 3 );
                REQUIRE( js.consumers( "EVENTS" ) == QStringList{ "EVENTS_READER" } );
            }
        }
    }
}

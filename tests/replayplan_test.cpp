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
 * @file replayplan_test.cpp
 * @brief Table-driven tests for planning a replay and for its step, without
 *        a server and without the clock.
 *
 * A replay plan is a pure function of the replay config and the stream's
 * state, and the step one of the (pending, stored time) of each message, so
 * each row is a config, a stream and what is expected of them.  That nats.c
 * is called as the plan says is replay_test.cpp's, against a server: one
 * test for each deliver policy.
 */

#include <catch2/catch.hpp>

#include "replay.h"

#include <limits>
#include <vector>

using nats_monitor::caughtUpOnSubscribing;
using nats_monitor::DeliverFrom;
using nats_monitor::describeReplay;
using nats_monitor::kMaxReplayTimeUs;
using nats_monitor::kMaxStreamSequence;
using nats_monitor::kMinReplayTimeUs;
using nats_monitor::planReplay;
using nats_monitor::readStartSequence;
using nats_monitor::readStartTime;
using nats_monitor::ReplayConfig;
using nats_monitor::ReplayPlan;
using nats_monitor::replayProblem;
using nats_monitor::replaySaveProblem;
using nats_monitor::ReplaySkip;
using nats_monitor::ReplayStart;
using nats_monitor::ReplayStep;
using nats_monitor::Retention;
using nats_monitor::StreamState;

namespace {

/// 2026-11-11 10:00:00 UTC, a whole second, in microseconds since the epoch.
constexpr qint64 kSecondUs = 1794391200000000LL;
constexpr qint64 kSecondNs = kSecondUs * 1000;

/// A stream with the messages @p first to @p last; none if @p first is 0
/// (a stream never written to) or @p last < @p first (one emptied).
StreamState stream( quint64 first, quint64 last, Retention retention = Retention::Limits )
{
    StreamState state;
    state.retention = retention;
    state.firstSequence = first;
    state.lastSequence = last;
    state.messages = first > 0 && last >= first ? last - first + 1 : 0;
    return state;
}

ReplayConfig replay( ReplayStart start = ReplayStart::All )
{
    ReplayConfig config;
    config.stream = "ORDERS";
    config.start = start;
    return config;
}

ReplayConfig last( quint64 count, const QString& filter = {} )
{
    auto config = replay( ReplayStart::Last );
    config.lastMessages = count;
    config.filterSubject = filter;
    return config;
}

ReplayConfig fromSequence( quint64 sequence )
{
    auto config = replay( ReplayStart::Sequence );
    config.sequence = sequence;
    return config;
}

ReplayConfig fromTime( qint64 timeUs )
{
    auto config = replay( ReplayStart::Time );
    config.timeUs = timeUs;
    return config;
}

/// Where a plan starts the consumer and what its step skips.
struct Start {
    DeliverFrom deliver = DeliverFrom::All;
    quint64 sequence = 0;
    qint64 timeNs = 0;
    quint64 skipWhilePending = 0;
    qint64 skipStoredBeforeNs = 0;
    bool caughtUpAtOnce = false;
};

Start startOf( const ReplayPlan& plan )
{
    return { plan.deliver,           plan.startSequence,       plan.startTimeNs,
             plan.skip.whilePending, plan.skip.storedBeforeNs, plan.caughtUpAtOnce };
}

bool operator==( const Start& a, const Start& b )
{
    return a.deliver == b.deliver && a.sequence == b.sequence && a.timeNs == b.timeNs
           && a.skipWhilePending == b.skipWhilePending
           && a.skipStoredBeforeNs == b.skipStoredBeforeNs && a.caughtUpAtOnce == b.caughtUpAtOnce;
}

/// A message as the step sees it, and what it makes of it.
struct Delivery {
    quint64 pending; ///< The consumer's NumPending: messages after this one.
    qint64 storedNs;
    bool written;
    bool caughtUp;
};

} // namespace

namespace Catch {
template <>
struct StringMaker<Start> {
    static std::string convert( const Start& s )
    {
        return "{ deliver " + std::to_string( static_cast<int>( s.deliver ) ) + ", sequence "
               + std::to_string( s.sequence ) + ", time " + std::to_string( s.timeNs )
               + ", skip while pending " + std::to_string( s.skipWhilePending ) + ", skip before "
               + std::to_string( s.skipStoredBeforeNs )
               + ( s.caughtUpAtOnce ? ", caught up at once }" : " }" );
    }
};
} // namespace Catch

SCENARIO( "a replay plan says where the consumer starts and what the step skips", "[replay][plan]" )
{
    struct Row {
        const char* what;
        ReplayConfig config;
        StreamState stream;
        Start expected;
    };
    const std::vector<Row> rows{
        { "all messages", replay(), stream( 1, 10 ), {} },
        { "the last 3", last( 3 ), stream( 1, 10 ), { DeliverFrom::StartSequence, 8 } },
        { "the last 3 of a stream that lost its first ones",
          last( 3 ),
          stream( 5, 10 ),
          { DeliverFrom::StartSequence, 8 } },
        { "the last 10 of ten: all", last( 10 ), stream( 1, 10 ), {} },
        { "the last 20 of ten: all", last( 20 ), stream( 1, 10 ), {} },
        { "the last 6 of the messages 5 to 10: all", last( 6 ), stream( 5, 10 ), {} },
        { "the last 2 on a subject: all, skipping while 2 or more are pending",
          last( 2, "orders.eu" ),
          stream( 1, 10 ),
          { DeliverFrom::All, 0, 0, 2 } },
        { "from sequence 7",
          fromSequence( 7 ),
          stream( 1, 10 ),
          { DeliverFrom::StartSequence, 7 } },
        { "from sequence 11, after the last: caught up at once",
          fromSequence( 11 ),
          stream( 1, 10 ),
          { DeliverFrom::StartSequence, 11, 0, 0, 0, true } },
        { "from a whole second",
          fromTime( kSecondUs ),
          stream( 1, 10 ),
          { DeliverFrom::StartTime, 0, kSecondNs } },
        { "from half a second in",
          fromTime( kSecondUs + 500000 ),
          stream( 1, 10 ),
          { DeliverFrom::StartTime, 0, kSecondNs + 500000000 } },
        { "from 0.1 s in, written right by nats.c 3.14",
          fromTime( kSecondUs + 100000 ),
          stream( 1, 10 ),
          { DeliverFrom::StartTime, 0, kSecondNs + 100000000 } },
        { "from 0.025 s in: at the whole second, skipping what is stored before",
          fromTime( kSecondUs + 25000 ),
          stream( 1, 10 ),
          { DeliverFrom::StartTime, 0, kSecondNs, 0, kSecondNs + 25000000 } },
        { "from 1 us in: at the whole second, skipping what is stored before",
          fromTime( kSecondUs + 1 ),
          stream( 1, 10 ),
          { DeliverFrom::StartTime, 0, kSecondNs, 0, kSecondNs + 1000 } },
        { "an empty stream: caught up at once",
          replay(),
          stream( 0, 0 ),
          { DeliverFrom::All, 0, 0, 0, 0, true } },
        { "an emptied stream, the last 5: caught up at once",
          last( 5 ),
          stream( 11, 10 ),
          { DeliverFrom::All, 0, 0, 0, 0, true } },
        { "an empty stream, from a time: caught up at once",
          fromTime( kSecondUs ),
          stream( 0, 0 ),
          { DeliverFrom::StartTime, 0, kSecondNs, 0, 0, true } },
    };
    for ( const auto& row : rows ) {
        DYNAMIC_SECTION( row.what )
        {
            const auto plan = planReplay( row.config, row.stream );
            REQUIRE( plan.refusal.isEmpty() );
            REQUIRE( plan.warning.isEmpty() );
            REQUIRE( startOf( plan ) == row.expected );
        }
    }
}

SCENARIO( "a replay plan names the consumer's filter, threshold and what follows catching up",
          "[replay][plan]" )
{
    GIVEN( "a replay of all subjects that ends once caught up" )
    {
        auto config = replay();
        config.inactiveThresholdMs = 500;
        const auto plan = planReplay( config, stream( 1, 10 ) );

        THEN( "the filter is >, the threshold in nanoseconds, and it ends at caught up" )
        {
            REQUIRE( plan.filter == ">" );
            REQUIRE( plan.inactiveThresholdNs == 500000000 );
            REQUIRE_FALSE( plan.followLive );
        }
    }

    GIVEN( "a replay of orders.eu.> that follows live, the threshold left to the server" )
    {
        auto config = replay();
        config.filterSubject = "orders.eu.>";
        config.followLive = true;
        config.inactiveThresholdMs = 0;
        const auto plan = planReplay( config, stream( 1, 10 ) );

        THEN( "the filter is the subject, no threshold, and it follows live once caught up" )
        {
            REQUIRE( plan.filter == "orders.eu.>" );
            REQUIRE( plan.inactiveThresholdNs == 0 );
            REQUIRE( plan.followLive );
        }
    }
}

SCENARIO( "a replay plan refuses a work queue and warns of interest retention",
          "[replay][plan][retention]" )
{
    GIVEN( "a work queue stream" )
    {
        auto config = replay();
        config.stream = "JOBS";
        const auto plan = planReplay( config, stream( 1, 3, Retention::WorkQueue ) );

        THEN( "it is refused, saying why" )
        {
            REQUIRE( plan.refusal
                     == "it is a work queue (work-queue retention), whose messages a replay "
                        "would take away from its workers" );
        }
    }

    GIVEN( "an interest stream" )
    {
        auto config = replay();
        config.stream = "EVENTS";
        const auto plan = planReplay( config, stream( 1, 3, Retention::Interest ) );

        THEN( "it is replayed, with a warning that names the stream" )
        {
            REQUIRE( plan.refusal.isEmpty() );
            REQUIRE( plan.warning
                     == "warning: stream EVENTS has interest retention: it holds only the "
                        "messages its consumers have yet to acknowledge, and the replay is one "
                        "more consumer while it runs" );
        }
    }

    GIVEN( "a limits stream" )
    {
        const auto plan = planReplay( replay(), stream( 1, 3 ) );

        THEN( "it is replayed without a warning" )
        {
            REQUIRE( plan.refusal.isEmpty() );
            REQUIRE( plan.warning.isEmpty() );
        }
    }

    GIVEN( "a replay that cannot start, from sequence 0" )
    {
        THEN( "the plan refuses it as replayProblem() does" )
        {
            REQUIRE( planReplay( fromSequence( 0 ), stream( 1, 3 ) ).refusal
                     == "A stream's sequences start at 1." );
        }
    }

    GIVEN( "a replay filtered on two subjects, or on one that is not a subject" )
    {
        THEN( "the plan refuses it as replayProblem() does" )
        {
            REQUIRE( planReplay( last( 2, "orders.eu\norders.us" ), stream( 1, 3 ) ).refusal
                     == "Enter one subject to replay, or none for all." );
            REQUIRE( planReplay( last( 2, "orders.>.eu" ), stream( 1, 3 ) ).refusal
                     == "orders.>.eu is not a valid subject: > must be the last token." );
        }
    }
}

SCENARIO( "a replay is named by its stream, subject, start and what follows", "[replay][plan]" )
{
    struct Row {
        const char* what;
        ReplayConfig config;
        const char* name;
    };
    auto onSubject = replay();
    onSubject.filterSubject = "orders.us";
    auto live = last( 100, "orders.eu.>" );
    live.followLive = true;
    const std::vector<Row> rows{
        { "all messages", replay(), "stream ORDERS" },
        { "all on a subject", onSubject, "stream ORDERS, subject orders.us" },
        { "the last 3", last( 3 ), "stream ORDERS, the last 3 messages" },
        { "the last one", last( 1 ), "stream ORDERS, the last message" },
        { "from sequence 7", fromSequence( 7 ), "stream ORDERS, from sequence 7" },
        { "from a time, as the lines write it", fromTime( kSecondUs + 25000 ),
          "stream ORDERS, from 2026-11-11 10:00:00.025000Z" },
        { "the last 100 on a subject, then live", live,
          "stream ORDERS, subject orders.eu.>, the last 100 messages, then live" },
    };
    for ( const auto& row : rows ) {
        DYNAMIC_SECTION( row.what )
        {
            REQUIRE( describeReplay( row.config ) == QString::fromUtf8( row.name ) );
        }
    }
}

SCENARIO( "a replay's start is read from its text, or kept as it was for the check",
          "[replay][checks]" )
{
    struct Row {
        const char* text;
        quint64 sequence; ///< When read.
        const char* unread;
    };
    const std::vector<Row> sequences{
        { "1", 1, "" },
        { "42", 42, "" },
        { "18446744073709551615", kMaxStreamSequence, "" },
        { "0", 0, "" },
        { "", 0, "" },
        { "18446744073709551616", 1, "18446744073709551616" },
        { "99999999999999999999", 1, "99999999999999999999" },
        { "-1", 1, "-1" },
        { "seven", 1, "seven" },
    };
    for ( const auto& row : sequences ) {
        DYNAMIC_SECTION( "the sequence \"" << row.text << "\"" )
        {
            auto config = fromSequence( 1 );
            readStartSequence( config, QString::fromUtf8( row.text ) );
            REQUIRE( config.unreadSequence == QString::fromUtf8( row.unread ) );
            REQUIRE( config.sequence == row.sequence );
        }
    }

    struct TimeRow {
        const char* text;
        qint64 timeUs; ///< When read.
        const char* unread;
    };
    const std::vector<TimeRow> times{
        { "1794391200000000", kSecondUs, "" },
        { "-5", -5, "" },
        { "9223372036854775807", std::numeric_limits<qint64>::max(), "" },
        { "", 0, "" },
        { "9223372036854775808", kSecondUs, "9223372036854775808" },
        { "yesterday", kSecondUs, "yesterday" },
    };
    for ( const auto& row : times ) {
        DYNAMIC_SECTION( "the time \"" << row.text << "\"" )
        {
            auto config = fromTime( kSecondUs );
            readStartTime( config, QString::fromUtf8( row.text ) );
            REQUIRE( config.unreadTime == QString::fromUtf8( row.unread ) );
            REQUIRE( config.timeUs == row.timeUs );
        }
    }
}

SCENARIO( "a replay's start that cannot be read, or is out of range, is refused",
          "[replay][checks]" )
{
    const QString timeRange = "The start time must be from 1970-01-01 00:00:01.000000Z to "
                              "2262-04-11 23:47:16.000000Z.";
    auto unreadSequence = fromSequence( 1 );
    unreadSequence.unreadSequence = "18446744073709551616";
    auto unreadTime = fromTime( kSecondUs );
    unreadTime.unreadTime = "yesterday";
    // An unread start of another kind than the replay's is not used.
    auto lastWithUnread = last( 3 );
    lastWithUnread.unreadSequence = "seven";
    lastWithUnread.unreadTime = "yesterday";
    auto timeWithUnreadSequence = fromTime( kSecondUs );
    timeWithUnreadSequence.unreadSequence = "seven";

    struct Row {
        const char* what;
        ReplayConfig config;
        QString problem; ///< Empty: none.
    };
    const std::vector<Row> rows{
        { "sequence 1", fromSequence( 1 ), {} },
        { "the largest sequence", fromSequence( kMaxStreamSequence ), {} },
        { "sequence 0", fromSequence( 0 ), "A stream's sequences start at 1." },
        { "one above the largest sequence", unreadSequence,
          "18446744073709551616 is not a stream sequence: they go from 1 to "
          "18446744073709551615." },
        { "the earliest time", fromTime( kMinReplayTimeUs ), {} },
        { "the latest time", fromTime( kMaxReplayTimeUs ), {} },
        { "a time", fromTime( kSecondUs ), {} },
        { "one microsecond before the earliest", fromTime( kMinReplayTimeUs - 1 ), timeRange },
        { "the time 0", fromTime( 0 ), timeRange },
        { "a negative time", fromTime( -kSecondUs ), timeRange },
        { "one microsecond after the latest", fromTime( kMaxReplayTimeUs + 1 ), timeRange },
        { "a time whose nanoseconds overflow", fromTime( std::numeric_limits<qint64>::max() ),
          timeRange },
        { "a time that is not a number", unreadTime,
          "yesterday is not a start time: one is a number of microseconds since 1970." },
        { "the last 3, with a sequence and a time that cannot be read", lastWithUnread, {} },
        { "a time, with a sequence that cannot be read", timeWithUnreadSequence, {} },
    };
    for ( const auto& row : rows ) {
        DYNAMIC_SECTION( row.what )
        {
            REQUIRE( replayProblem( row.config ) == row.problem );
            REQUIRE( planReplay( row.config, stream( 1, 10 ) ).refusal == row.problem );
        }
    }
}

SCENARIO( "a connection that chose to replay is saved only with a stream", "[replay][checks]" )
{
    const QString noStream
        = "Enter the stream to replay, or List the server's streams to choose it.";
    ReplayConfig withStream;
    withStream.stream = "ORDERS";

    GIVEN( "replay chosen" )
    {
        THEN( "it needs a stream, any stream: the rest is replayProblem()'s" )
        {
            REQUIRE( replaySaveProblem( true, ReplayConfig{} ) == noStream );
            REQUIRE( replaySaveProblem( true, withStream ).isEmpty() );
            REQUIRE( replaySaveProblem( true, fromSequence( 0 ) ).isEmpty() );
        }
    }

    GIVEN( "replay not chosen" )
    {
        THEN( "it needs none" )
        {
            REQUIRE( replaySaveProblem( false, ReplayConfig{} ).isEmpty() );
            REQUIRE( replaySaveProblem( false, withStream ).isEmpty() );
        }
    }
}

SCENARIO( "a replay's step skips and catches up message by message", "[replay][plan]" )
{
    struct Row {
        const char* what;
        ReplaySkip skip;
        std::vector<Delivery> deliveries;
    };
    constexpr qint64 t = kSecondNs; // the start time of a skip by time
    const std::vector<Row> rows{
        { "no skip: each is written, the one with none pending catches up",
          {},
          { { 2, t, true, false }, { 1, t, true, false }, { 0, t, true, true } } },
        { "the last 2 on a subject: skipped while 2 or more are pending",
          { 2, 0 },
          { { 4, t, false, false },
            { 3, t, false, false },
            { 2, t, false, false },
            { 1, t, true, false },
            { 0, t, true, true } } },
        { "the last 2: once one is written, all are, live ones too",
          { 2, 0 },
          { { 1, t, true, false }, { 0, t, true, true }, { 5, t, true, false } } },
        { "from a time: skipped while stored before it, then all",
          { 0, t + 25000000 },
          { { 3, t, false, false },
            { 2, t + 24999999, false, false },
            { 1, t + 25000000, true, false },
            { 0, t + 1000, true, true } } },
        { "from a time after all there was: the last skipped catches up",
          { 0, t + 25000000 },
          { { 1, t, false, false }, { 0, t + 1000, false, true } } },
    };
    for ( const auto& row : rows ) {
        DYNAMIC_SECTION( row.what )
        {
            ReplayStep step( row.skip );
            for ( const auto& delivery : row.deliveries ) {
                INFO( "pending " << delivery.pending << ", stored " << delivery.storedNs );
                const auto outcome = step.next( delivery.pending, delivery.storedNs );
                REQUIRE( outcome.write == delivery.written );
                REQUIRE( outcome.caughtUp == delivery.caughtUp );
            }
        }
    }
}

SCENARIO( "a replay with nothing to deliver is caught up once it subscribed", "[replay][plan]" )
{
    const auto plan = planReplay( replay(), stream( 1, 10 ) );
    const auto empty = planReplay( replay(), stream( 0, 0 ) );

    GIVEN( "a consumer with nothing pending that delivered nothing" )
    {
        THEN( "it is caught up" )
        {
            REQUIRE( caughtUpOnSubscribing( plan, 0, 0 ) );
        }
    }

    GIVEN( "a consumer with messages pending, or some delivered already" )
    {
        THEN( "the messages will say when it is caught up" )
        {
            REQUIRE_FALSE( caughtUpOnSubscribing( plan, 3, 0 ) );
            REQUIRE_FALSE( caughtUpOnSubscribing( plan, 0, 3 ) );
        }
    }

    GIVEN( "a plan for an empty stream" )
    {
        THEN( "it is caught up, whatever the consumer says" )
        {
            REQUIRE( caughtUpOnSubscribing( empty, 3, 0 ) );
        }
    }
}

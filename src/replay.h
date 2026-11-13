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
 * @file replay.h
 * @brief A replay as the user sets it up and as it is planned: where it may
 *        start, how it is named, what makes it unusable, where its consumer
 *        starts and which messages it writes.
 *
 * The session replays (natssession.h, REPLAY); this is what the dialog,
 * the profiles, the tiles, the notifications and the session need of a
 * ReplayConfig.  All of it is pure: no nats.c, no server, no clock.  Before
 * it subscribes the replay's connection path (replaysubscription.h) plans
 * the replay from the stream's state (planReplay()), turns the plan into
 * nats.c's consumer options, and hands each message's pending count and
 * stored time to the plan's ReplayStep.
 *
 * A new start position is a row in kReplayStarts, a case in describeReplay()
 * and one in planReplay(); with a new DeliverFrom, the connection path maps
 * it to nats.c's deliver policy too (ReplaySubscription::subscribe()).  The
 * streams to choose from are listed apart (streams.h): that connects.
 */

#pragma once

#include "sessionconfig.h"

#include <QByteArray>
#include <QString>

#include <array>
#include <limits>

namespace nats_monitor {

struct ReplayStartInfo {
    ReplayStart start;
    const char* savedName; ///< In a profile: a word, so nats.ini stays readable.
    const char* label;     ///< In the dialog's list.
};

inline constexpr std::array<ReplayStartInfo, 4> kReplayStarts{ {
    { ReplayStart::All, "all", "All messages" },
    { ReplayStart::Last, "last", "Last messages" },
    { ReplayStart::Sequence, "sequence", "From sequence" },
    { ReplayStart::Time, "time", "From time (UTC)" },
} };

/// The row of kReplayStarts for @p start.
inline const ReplayStartInfo& replayStartInfo( ReplayStart start )
{
    for ( const auto& info : kReplayStarts ) {
        if ( info.start == start ) {
            return info;
        }
    }
    return kReplayStarts.front();
}

/// The largest stream sequence: a stream counts its messages in 64 bits.
inline constexpr quint64 kMaxStreamSequence = std::numeric_limits<quint64>::max();

/// The most messages a replay of the last ones can replay: a stream holds
/// no more than it has sequences.
inline constexpr quint64 kMaxLastMessages = kMaxStreamSequence;

/// The earliest start time of a replay, in microseconds since the epoch:
/// 1970-01-01 00:00:01 UTC.  nats.c sends the time 0 as no time at all, and
/// the plan may round a start down to its whole second (planReplay()).
inline constexpr qint64 kMinReplayTimeUs = 1000000;

/// The latest start time of a replay, in microseconds since the epoch: the
/// last whole second whose nanoseconds, as the consumer is given the time,
/// a qint64 holds (2262-04-11 23:47:16 UTC).
inline constexpr qint64 kMaxReplayTimeUs
    = std::numeric_limits<qint64>::max() / 1000000000 * 1000000;

/// Read @p text, a decimal number, as the start sequence of @p replay: the
/// dialog's field, or nats.ini's value.  One that is not a number from 0 to
/// kMaxStreamSequence is kept as it was (ReplayConfig::unreadSequence), for
/// replayProblem() to refuse; an empty one is the sequence 0.
void readStartSequence( ReplayConfig& replay, const QString& text );

/// Read @p text, a decimal number of microseconds since the epoch, as the
/// start time of @p replay, as nats.ini holds it.  One that is not a number
/// a qint64 holds is kept as it was (ReplayConfig::unreadTime), for
/// replayProblem() to refuse; an empty one is the time 0.
void readStartTime( ReplayConfig& replay, const QString& text );

/// Read @p text, a decimal number, as how many last messages @p replay
/// replays: the dialog's field, or nats.ini's value.  One that is not a
/// number from 0 to kMaxLastMessages is kept as it was
/// (ReplayConfig::unreadLastMessages), for replayProblem() to refuse; an
/// empty one is 0.
void readLastMessages( ReplayConfig& replay, const QString& text );

/// @p replay as the marker lines and the dialog name it: "stream ORDERS,
/// subject orders.eu.>, the last 100 messages, then live".
QString describeReplay( const ReplayConfig& replay );

/// Why @p replay, enabled, cannot start, for the user; empty if it can: no
/// stream, a number of last messages that is 0 or cannot be read, a start
/// sequence that is 0 or cannot be
/// read, a start time that cannot be read or is not from kMinReplayTimeUs
/// to kMaxReplayTimeUs, a filter that is not one subject.  Only the start
/// the replay has is checked.
/// What the config alone tells: the stream's own rules are the plan's
/// (ReplayPlan::refusal).  The start check of a connection makes it
/// (startProblem(), profiles.h).
QString replayProblem( const ReplayConfig& replay );

/// Why a connection whose user chose to replay (@p chosen) cannot be saved
/// with @p replay, for the user; empty if it can: replay chosen but no
/// stream entered.  The save check of a connection makes it before its
/// other rules (saveProblem(), profiles.h).
QString replaySaveProblem( bool chosen, const ReplayConfig& replay );

/// How a stream keeps its messages (js_RetentionPolicy).
enum class Retention {
    Limits,    ///< Until its limits remove them: a replay takes nothing away.
    Interest,  ///< Until each of its consumers acknowledged them.
    WorkQueue, ///< Until one consumer acknowledged them: a replay would take them.
};

/// What a replay is planned from: the stream as the server tells it
/// before the replay subscribes (js_GetStreamInfo).
struct StreamState {
    Retention retention = Retention::Limits;
    quint64 firstSequence = 0;
    quint64 lastSequence = 0;
    quint64 messages = 0;
};

/// Where the replay's consumer starts (its deliver policy).
enum class DeliverFrom {
    All,           ///< At the stream's first message (that matches the filter).
    StartSequence, ///< At ReplayPlan::startSequence.
    StartTime,     ///< At the first message stored at or after ReplayPlan::startTimeNs.
};

/// Which of the messages the consumer delivers the replay skips, until the
/// first one it writes; from then on it writes all.
struct ReplaySkip {
    /// Skip while the consumer has at least this many more pending: the
    /// last N with a filter, whose matches only the server knows.  0: none.
    quint64 whilePending = 0;
    /// Skip what was stored before this time (nanoseconds since the epoch,
    /// UTC): a start time the consumer is given rounded down.  0: none.
    qint64 storedBeforeNs = 0;
};

/// A replay as it will run on its stream: what planReplay() decided.
struct ReplayPlan {
    /// Why the replay does not subscribe, for the user ("replaying … failed:
    /// <refusal>"); empty if it does.  Nothing else in a refused plan counts.
    QString refusal;
    /// A marker line to write before the messages; empty for none.
    QString warning;
    /// The consumer's filter subject: the config's, or > for all.
    QByteArray filter;
    DeliverFrom deliver = DeliverFrom::All;
    quint64 startSequence = 0; ///< For DeliverFrom::StartSequence.
    qint64 startTimeNs = 0;    ///< For DeliverFrom::StartTime: since the epoch, UTC.
    /// The consumer's inactive threshold; 0 leaves it to the server.
    qint64 inactiveThresholdNs = 0;
    ReplaySkip skip; ///< For the ReplayStep.
    /// Nothing to replay, as the stream's state shows (an empty stream, a
    /// start after its last sequence): caught up as soon as subscribed.
    bool caughtUpAtOnce = false;
    /// Once caught up, go on live; otherwise the replay is complete then.
    bool followLive = false;
};

/// The replay of @p replay on the stream @p stream: refused, or where its
/// consumer starts, what it skips, and whether it is caught up at once.
ReplayPlan planReplay( const ReplayConfig& replay, const StreamState& stream );

/// Whether the replay of @p plan is caught up once it subscribed, before any
/// message came: the plan says so, or the consumer (its info) has nothing
/// pending and delivered nothing, so no message will say it.
bool caughtUpOnSubscribing( const ReplayPlan& plan, quint64 consumerPending,
                            quint64 consumerDelivered );

/**
 * What a replay does with each message its consumer delivers: write or skip
 * it (ReplayPlan::skip), and whether the replay has caught up with it.  One
 * for each replay, called in the order of delivery, from one thread.
 */
class ReplayStep {
public:
    struct Outcome {
        bool write = false;    ///< Write the message; false: skip it.
        bool caughtUp = false; ///< It was the last one there was (none pending).
    };

    explicit ReplayStep( const ReplaySkip& skip = {} )
        : skip_( skip )
    {
    }

    /// The next message: @p pending more after it (the consumer's
    /// NumPending), stored at @p storedNs (nanoseconds since the epoch, UTC).
    Outcome next( quint64 pending, qint64 storedNs );

private:
    ReplaySkip skip_; ///< Cleared at the first message written.
};

} // namespace nats_monitor

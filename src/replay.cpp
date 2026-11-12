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
 * @file replay.cpp
 * @brief Implementation of replay.h: describing, checking and planning a
 *        replay, and its step.
 */

#include "replay.h"

#include "lineformat.h"
#include "subjects.h"

namespace nats_monitor {

namespace {

constexpr qint64 kNsPerSecond = 1000000000;
constexpr qint64 kNsPerMs = 1000000;

} // namespace

QString describeReplay( const ReplayConfig& replay )
{
    QString text = QStringLiteral( "stream %1" ).arg( replay.stream );
    if ( !replay.filterSubject.isEmpty() ) {
        text += QStringLiteral( ", subject %1" ).arg( replay.filterSubject );
    }
    switch ( replay.start ) {
    case ReplayStart::All:
        break;
    case ReplayStart::Last:
        text += replay.lastMessages == 1
                    ? QStringLiteral( ", the last message" )
                    : QStringLiteral( ", the last %1 messages" ).arg( replay.lastMessages );
        break;
    case ReplayStart::Sequence:
        text += QStringLiteral( ", from sequence %1" ).arg( replay.sequence );
        break;
    case ReplayStart::Time:
        // The time as the lines have it.
        text += QStringLiteral( ", from %1" )
                    .arg( QString::fromLatin1( formatTimestamp( replay.timeUs ) ) );
        break;
    }
    if ( replay.followLive ) {
        text += QStringLiteral( ", then live" );
    }
    return text;
}

QString replayProblem( const ReplayConfig& replay )
{
    if ( replay.stream.trimmed().isEmpty() ) {
        return QStringLiteral( "No stream to replay." );
    }
    if ( replay.start == ReplayStart::Last && replay.lastMessages == 0 ) {
        return QStringLiteral( "Replaying the last 0 messages replays nothing." );
    }
    if ( replay.start == ReplayStart::Sequence && replay.sequence == 0 ) {
        return QStringLiteral( "A stream's sequences start at 1." );
    }
    if ( !replay.filterSubject.isEmpty() ) {
        const auto parsed = parseSubscriptions( replay.filterSubject );
        if ( !parsed.error.isEmpty() ) {
            return parsed.error;
        }
        if ( parsed.subscriptions.size() != 1
             || !parsed.subscriptions.front().queueGroup.isEmpty() ) {
            return QStringLiteral( "Enter one subject to replay, or none for all." );
        }
    }
    return {};
}

QString replaySaveProblem( bool chosen, const ReplayConfig& replay )
{
    if ( chosen && !replay.enabled() ) {
        return QStringLiteral(
            "Enter the stream to replay, or List the server's streams to choose it." );
    }
    return {};
}

ReplayPlan planReplay( const ReplayConfig& replay, const StreamState& stream )
{
    ReplayPlan plan;
    plan.refusal = replayProblem( replay );
    if ( !plan.refusal.isEmpty() ) {
        return plan;
    }
    if ( stream.retention == Retention::WorkQueue ) {
        // A work queue's consumer takes the messages it reads away from the
        // workers; the server allows none that does not acknowledge.
        plan.refusal = QStringLiteral( "it is a work queue (work-queue retention), whose messages "
                                       "a replay would take away from its workers" );
        return plan;
    }
    if ( stream.retention == Retention::Interest ) {
        plan.warning = QStringLiteral( "warning: stream %1 has interest retention: it holds only "
                                       "the messages its consumers have yet to acknowledge, and "
                                       "the replay is one more consumer while it runs" )
                           .arg( replay.stream );
    }

    plan.filter
        = replay.filterSubject.isEmpty() ? QByteArray( ">" ) : replay.filterSubject.toUtf8();
    plan.inactiveThresholdNs = static_cast<qint64>( replay.inactiveThresholdMs ) * kNsPerMs;
    plan.followLive = replay.followLive;
    plan.caughtUpAtOnce = stream.messages == 0;

    const auto first = stream.firstSequence;
    const auto last = stream.lastSequence;
    switch ( replay.start ) {
    case ReplayStart::All:
        break;
    case ReplayStart::Last:
        if ( replay.filterSubject.isEmpty() ) {
            if ( last >= replay.lastMessages && last - replay.lastMessages + 1 > first ) {
                plan.deliver = DeliverFrom::StartSequence;
                plan.startSequence = last - replay.lastMessages + 1;
            }
        }
        else {
            // Which of the stream's messages match is the server's to
            // know: read them all, write the last N.
            plan.skip.whilePending = replay.lastMessages;
        }
        break;
    case ReplayStart::Sequence:
        plan.deliver = DeliverFrom::StartSequence;
        plan.startSequence = replay.sequence;
        plan.caughtUpAtOnce = plan.caughtUpAtOnce || replay.sequence > last;
        break;
    case ReplayStart::Time: {
        plan.deliver = DeliverFrom::StartTime;
        // nats.c (3.14) writes the fraction of a second without its leading
        // zeros, so 12:00:00.025 would reach the server as 12:00:00.25, past
        // what was stored in between.  Such a time starts the consumer at
        // its whole second, and what the consumer has from before the time
        // is skipped.  nats.c's main has the fix (974c232), after 3.14.0;
        // with a release of it this can go.
        const qint64 startNs = replay.timeUs * 1000;
        const qint64 fractionNs = ( ( startNs % kNsPerSecond ) + kNsPerSecond ) % kNsPerSecond;
        if ( fractionNs > 0 && fractionNs < kNsPerSecond / 10 ) {
            plan.startTimeNs = startNs - fractionNs;
            plan.skip.storedBeforeNs = startNs;
        }
        else {
            plan.startTimeNs = startNs;
        }
        break;
    }
    }
    return plan;
}

bool caughtUpOnSubscribing( const ReplayPlan& plan, quint64 consumerPending,
                            quint64 consumerDelivered )
{
    return plan.caughtUpAtOnce || ( consumerPending == 0 && consumerDelivered == 0 );
}

ReplayStep::Outcome ReplayStep::next( quint64 pending, qint64 storedNs )
{
    Outcome outcome;
    outcome.caughtUp = pending == 0;
    if ( skip_.whilePending > 0 && pending >= skip_.whilePending ) {
        return outcome; // before the last N
    }
    if ( storedNs < skip_.storedBeforeNs ) {
        return outcome; // before the start time
    }
    skip_ = {}; // from the first written on, all are
    outcome.write = true;
    return outcome;
}

} // namespace nats_monitor

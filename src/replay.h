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
 * @brief A replay as the user sets it up: where it may start, how it is
 *        named, what makes it unusable, and the streams to choose from.
 *
 * The session replays (natssession.h, REPLAY); this is what the dialog,
 * the profiles, the tiles and the notifications need of a ReplayConfig.
 * A new start position is a row in kReplayStarts, a case in describeReplay()
 * and one in the session's subscribe (natssession.cpp).
 */

#pragma once

#include "natssession.h"

#include <QString>
#include <QStringList>

#include <array>

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

/// @p replay as the marker lines and the dialog name it: "stream ORDERS,
/// subject orders.eu.>, the last 100 messages, then live".
QString describeReplay( const ReplayConfig& replay );

/// Why @p replay, enabled, cannot start, for the user; empty if it can.
/// The filter subject is checked where it is entered (profiles.cpp).
QString replayProblem( const ReplayConfig& replay );

/**
 * The names of the JetStream streams on the server of @p config, sorted
 * ($JS.API.STREAM.NAMES), to choose the one to replay.  Connects with the
 * config's URL, auth and TLS, and waits for the answer: call it off the
 * GUI thread.  It takes at most 2 s for each server URL to connect to
 * (one after the other, should several not answer) and 2 s for each
 * answer of the server.
 *
 * @return the names; empty, with @p error set (for the user), if they
 *         could not be listed.  An empty list without error: no stream.
 */
QStringList listStreams( const SessionConfig& config, QString* error );

} // namespace nats_monitor

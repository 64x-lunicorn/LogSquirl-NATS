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
 * @file sessionstatus.h
 * @brief A session's transitions, and the status of a connection kept from
 *        them.
 *
 * A session (natssession.h) reports each step of its life as a typed
 * Transition: connecting, running at a server, reconnecting, failed with a
 * reason, completed, stopped.  Nobody reads a state out of its texts.
 *
 * Sessions (sessions.h) keeps a ConnectionStatus for each connection: the
 * state the transitions lead to, the detail of the last one, the server,
 * the file, and the counts and msgs/s it samples from the session.  The
 * time of a sample is given, so the rate is computed without a clock of its
 * own.  The sidebar only shows the SessionStatus it holds.
 *
 * Pure: no NATS, no thread, no clock.
 */

#pragma once

#include <QString>

namespace nats_monitor {

/// A step of a session's life, as the session reports it: one for each.
struct Transition {
    enum class Kind {
        Connecting,   ///< Started: it connects and subscribes.
        Running,      ///< Connected, or connected again, and subscribed: to server.
        Reconnecting, ///< The connection broke; nats.c connects again.
        Failed,       ///< Ended on its own: it could not start, connect or subscribe,
                      ///< or the connection was closed for good; detail says why.
        Completed,    ///< A replay that does not follow live has caught up and ended.
        Stopped,      ///< stop() ended it.
    };
    Kind kind = Kind::Stopped;
    /// In words, as in the file: "connecting to …", "connected to …",
    /// "disconnected", why it failed, "replay complete", "stopped".
    QString detail;
    QString server; ///< Running: the URL it is connected to (redacted).

    bool operator==( const Transition& other ) const
    {
        return kind == other.kind && detail == other.detail && server == other.server;
    }
};

/// Where a connection's session is.
enum class SessionState {
    Stopped,    ///< Never started, stopped by the user, or a replay complete.
    Connecting, ///< Started and not connected yet, or reconnecting.
    Running,    ///< Connected and subscribed.
    Failed,     ///< Ended on its own: it could not start or connect, or was closed for good.
};

/// What a connection's session does, or did last.
struct SessionStatus {
    SessionState state = SessionState::Stopped;
    QString detail;   ///< The last transition, or error, in words; why it failed.
    QString server;   ///< The server it is connected to (redacted); empty while not connected.
    QString filePath; ///< The file it writes, or wrote last; empty if never started.
    int files = 0;    ///< How many files the capture has written: 1 + its rotations.
    quint64 received = 0;
    quint64 dropped = 0;
    quint64 rate = 0; ///< msgs/s between the last two samples; 0 once it no longer runs.
};

/// The status of one connection, as its session's transitions and the
/// samples of its counts make it.
class ConnectionStatus {
public:
    /// Apply @p transition.  Connecting starts afresh: no file, nothing
    /// counted, no rate.
    void apply( const Transition& transition );

    /// The capture writes @p path, its first file.
    void opened( const QString& path );

    /// The capture goes on in @p path, its next file.
    void rotated( const QString& path );

    /// The session reports an error and runs on: @p text is the detail now.
    void errorReported( const QString& text );

    /// The session's counts at @p nowMs (any monotonic clock, in
    /// milliseconds): the rate is what was received since the sample before,
    /// per second.  The first sample after Connecting only starts the rate;
    /// a sample at no later time leaves it as it is.
    void sample( quint64 received, quint64 dropped, qint64 nowMs );

    const SessionStatus& status() const
    {
        return status_;
    }

private:
    SessionStatus status_;
    bool sampled_ = false; ///< A sample since Connecting: the rate counts from it.
    quint64 sampledReceived_ = 0;
    qint64 sampledAtMs_ = 0;
};

} // namespace nats_monitor

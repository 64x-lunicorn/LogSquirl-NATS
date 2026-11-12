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
 * @file sessions.h
 * @brief The captures, one session per saved connection.
 *
 * Sessions starts a NatsSession for a profile (profiles.h), opens its file
 * in a followed tab, opens each next file of the capture when it rotates,
 * and keeps what the sidebar shows of it: its state, the server it is
 * connected to, its file and its counts.  A connection runs one session at
 * a time; several connections run side by side, each with its own
 * connection, file and tab.
 *
 * What a session did stays known after it was stopped or failed, until the
 * connection is started again: the sidebar shows why it failed and what it
 * captured.
 *
 * Starting, stopping (with notify) and errors are host notifications, as
 * in the Serial plugin; stopAll() (shutdown) notifies nothing.  A session's
 * file is kept for its tab when the session ends; the plugin removes the
 * temporary files when LogSquirl quits.  Each session writes below the
 * temporary root Sessions was created with: the plugin's (tempRoot(),
 * plugin.h).
 *
 * OWNERSHIP
 * ─────────
 * Created by logsquirl_plugin_init(), deleted by logsquirl_plugin_shutdown()
 * after stopAll().  The running NatsSessions are its children.
 */

#pragma once

#include <QHash>
#include <QObject>
#include <QString>

namespace nats_monitor {

class NatsSession;
struct ConnectionProfile;

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
    QString detail;   ///< The last connection event, or why it failed.
    QString server;   ///< The server it is connected to (redacted); empty while not connected.
    QString filePath; ///< The file it writes, or wrote last; empty if never started.
    int files = 0;    ///< How many files the capture has written: 1 + its rotations.
    quint64 received = 0;
    quint64 dropped = 0;
};

class Sessions : public QObject {
    Q_OBJECT

public:
    /// Sessions whose temporary directories go below @p tempRoot (tempdirs.h).
    explicit Sessions( const QString& tempRoot, QObject* parent = nullptr );
    ~Sessions() override;

    /**
     * Start a session for @p profile, with its password or token in
     * profile.auth.secret, and open its file in a followed tab.  No-op if
     * the connection's session runs already.
     *
     * @return whether a session runs now; if not, status() says why.
     */
    bool start( const ConnectionProfile& profile );

    /// Stop the session of the connection @p name and, with @p notify, say
    /// so in a host notification.  No-op if it does not run.
    void stop( const QString& name, bool notify = true );

    /// Stop every session, keeping their files for the tabs, and with
    /// @p notify say so for each in a host notification.
    void stopAll( bool notify = false );

    /// Whether the session of the connection @p name runs.
    bool isRunning( const QString& name ) const;

    /// Whether some session runs.
    bool anyRunning() const;

    /// What the session of the connection @p name does, or did last.
    SessionStatus status( const QString& name ) const;

Q_SIGNALS:
    /// The session of the connection @p name started, changed state, or ended.
    void changed( const QString& name );

private:
    struct Entry {
        NatsSession* session = nullptr; ///< While it runs.
        SessionStatus status;
    };

    /// Let go of the session of @p name, which was stopped or ended on its
    /// own: keep its file and counts, delete it later, and set @p state and,
    /// unless empty, @p detail.
    void release( const QString& name, SessionState state, const QString& detail = {} );

    const QString tempRoot_; ///< Given to each session it starts.
    QHash<QString, Entry> entries_;
};

} // namespace nats_monitor

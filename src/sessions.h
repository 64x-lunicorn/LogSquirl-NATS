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
 * and is the one owner of each connection's status (sessionstatus.h): the
 * state its session's transitions lead to, the server it is connected to,
 * its file, its counts and its msgs/s.  A connection runs one session at a
 * time; several connections run side by side, each with its own
 * connection, file and tab.
 *
 * The counts are sampled once a second (sample()), and changed() says so
 * when they moved; a transition is announced at once, once.  The rate is
 * computed on the clock Sessions was given: a test gives its own.
 *
 * What a session did stays known after it was stopped or failed, until the
 * connection is started again: the sidebar shows why it failed and what it
 * captured.
 *
 * Sessions reaches the host only through the host adapter it was created
 * with (hostadapter.h): the plugin's (pluginHost(), plugin.h), or a test's
 * in-memory one; never through the plugin's global state.  Starting,
 * stopping (with notify) and errors are host notifications, as in the
 * Serial plugin; stopAllAndWait() (shutdown) notifies nothing.  A stop
 * returns at once and is notified once the session has stopped; until then
 * the connection is stopping.  A rotation opens a
 * followed tab and writes to the host log; so does a failure, and a
 * certificate not verified (insecure).  A session's file is kept for its
 * tab when the session ends; the plugin removes the temporary files when
 * LogSquirl quits.  Each session writes below the temporary root Sessions
 * was created with: the plugin's (tempRoot(), plugin.h).
 *
 * OWNERSHIP
 * ─────────
 * Created by logsquirl_plugin_init(), deleted by logsquirl_plugin_shutdown()
 * after stopAllAndWait().  The running NatsSessions are its children.  The host
 * adapter is not owned: it outlives Sessions.
 */

#pragma once

#include "sessionstatus.h"

#include <QHash>
#include <QObject>
#include <QString>

#include <functional>

class QTimer;

namespace nats_monitor {

class HostAdapter;
class NatsSession;
struct ConnectionProfile;

class Sessions : public QObject {
    Q_OBJECT

public:
    /// A monotonic clock in milliseconds, which the rate is computed on.
    using Clock = std::function<qint64()>;

    /// Sessions that reach the host through @p host, whose temporary
    /// directories go below @p tempRoot (tempdirs.h), computing the rate on
    /// @p clock (the steady clock if none).
    Sessions( HostAdapter& host, const QString& tempRoot, QObject* parent = nullptr,
              Clock clock = {} );
    ~Sessions() override;

    /**
     * Start a session for @p profile, with its password or token in
     * profile.auth.secret, and open its file in a followed tab.  No-op if
     * the connection's session runs already; refused (false, the status
     * left as it is) while it is stopping.
     *
     * Started or not, profile.auth.secret is wiped (secrets.h) on return:
     * the session's worker has its own copy, for the connection options.
     *
     * @return whether a session runs now; if not, status() says why.
     */
    bool start( ConnectionProfile& profile );

    /// Stop the session of the connection @p name and, with @p notify, say
    /// so in a host notification once it has stopped.  Returns at once: the
    /// connection is stopping until its session has ended (natssession.h,
    /// STOPPING).  No-op if it does not run or is stopping already.
    void stop( const QString& name, bool notify = true );

    /// Stop every session, keeping their files for the tabs, and with
    /// @p notify say so for each in a host notification once it has
    /// stopped.  Returns at once, as stop() does.
    void stopAll( bool notify = false );

    /// Stop every session, as stopAll() does without notifying, and wait
    /// until each has ended: they stop side by side, so this waits for the
    /// slowest, not for their sum.  For the plugin's shutdown, before the
    /// library is unloaded, and the destructor.
    void stopAllAndWait();

    /// Whether the session of the connection @p name runs, and is not
    /// stopping.
    bool isRunning( const QString& name ) const;

    /// Whether the session of the connection @p name is stopping: it cannot
    /// be started again, nor the connection saved or deleted, until it has
    /// ended.
    bool isStopping( const QString& name ) const;

    /// Whether some session runs and is not stopping: what Stop all stops.
    bool anyRunning() const;

    /// What the session of the connection @p name does, or did last, with
    /// the counts and the rate of the last sample: whether it replays as it
    /// was started, not as its profile is saved now.
    SessionStatus status( const QString& name ) const;

    /// Sample the counts of each running session now, on the clock; called
    /// once a second.  changed() for each whose counts or rate moved.
    void sample();

Q_SIGNALS:
    /// The status of the connection @p name changed: a transition of its
    /// session, the next file, an error, or its counts.
    void changed( const QString& name );

private:
    struct Entry {
        NatsSession* session = nullptr; ///< While it runs, stopping included.
        ConnectionStatus status;
        bool notifyStopped = false; ///< stop() was asked to notify once it has stopped.
        bool replay = false;        ///< It was started as a replay (SessionStatus::replay).
    };

    /// Apply @p transition, which the session of @p name reported; an end
    /// lets go of the session.
    void onTransition( const QString& name, const Transition& transition );

    /// Sample the counts of the session of @p entry, if it runs.
    void sampleCounts( Entry& entry );

    /// Let go of the session of @p entry, which ended: keep its counts and
    /// its file (if it wrote one), and delete it later.
    void release( Entry& entry );

    HostAdapter& host_;      ///< Not owned.
    const QString tempRoot_; ///< Given to each session it starts.
    const Clock clock_;
    QTimer* timer_;
    QHash<QString, Entry> entries_;
};

} // namespace nats_monitor

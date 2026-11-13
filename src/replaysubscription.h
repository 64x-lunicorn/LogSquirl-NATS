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
 * @file replaysubscription.h
 * @brief A replay's connection path: nats.c's side of replaying a stream.
 *
 * Beside the replay's pure plan (replay.h), this is what a replay does on
 * the connection: it reads the stream's info, plans the replay from it
 * (planReplay()), subscribes nats.c's ordered consumer (js_Subscribe,
 * jsSubOptions::Ordered) as the plan says, takes each replayed message's
 * stored time and stream sequence from its metadata, hands it to the
 * plan's ReplayStep, and tells when the replay has caught up.  The session
 * (natssession.cpp) keeps what a capture and a replay share: the
 * connection, the subscription once made (its pending limits, drain and
 * end), the drops and the sink; it reaches them through an Outlet.
 * Tested through the session (replay_test.cpp).
 *
 * A new DeliverFrom of the plan is mapped to nats.c's deliver policy here
 * (subscribe()).
 */

#pragma once

// First: on Windows nats.h includes winsock2.h, which must come before any
// windows.h.
#include <nats.h>

#include "lineformat.h"
#include "replay.h"
#include "sessionconfig.h"

#include <QString>

#include <mutex>

namespace nats_monitor {

class ReplaySubscription {
public:
    /**
     * What the replay hands on to its session.  marker() and completed()
     * may come from the connection thread or nats.c's delivery thread;
     * subscribed() from the connection thread; discarding() and message()
     * from the delivery thread.
     */
    class Outlet {
    public:
        /// The replay subscribed as @p subscription, which nats.c may already
        /// deliver to: give it its completion callback and the pending
        /// limits.  NATS_OK, or why that failed (the replay fails then).
        virtual natsStatus subscribed( natsSubscription* subscription ) = 0;

        /// Whether the session stops: what nats.c still delivers is
        /// discarded, uncounted (the stream keeps it), and the step not
        /// taken.
        virtual bool discarding() const = 0;

        /// Write @p message, a replayed message of @p subscription (which
        /// nats.c may deliver before subscribed() is told of it).  Its views
        /// are valid until message() returns.
        virtual void message( natsSubscription* subscription, const Message& message ) = 0;

        /// Write @p text as a status marker: the plan's warning, or "caught
        /// up, following live".
        virtual void marker( const QString& text ) = 0;

        /// The replay caught up, not following live: the capture ends.
        /// Once at most.
        virtual void completed() = 0;

    protected:
        ~Outlet() = default;
    };

    /// The replay of @p replay, handing on to @p outlet, which outlives it.
    ReplaySubscription( const ReplayConfig& replay, Outlet& outlet );

    /// Destroys the JetStream context.  nats.c's callbacks use the object:
    /// it goes only once nats.c confirmed its last (the session leaks it
    /// otherwise, with the rest of what they use).
    ~ReplaySubscription();

    ReplaySubscription( const ReplaySubscription& ) = delete;
    ReplaySubscription& operator=( const ReplaySubscription& ) = delete;

    /**
     * Plan the replay on @p connection from its stream's info and subscribe
     * as the plan says, as @p subscription, which the session then owns
     * (drains and destroys).  Once at most, on the connection thread.  The
     * plan's warning is a marker before the messages; with nothing to
     * replay the replay is caught up at once.
     *
     * @return empty, or why it failed (for "replaying … failed: <why>"): the
     *         stream could not be read, the plan refused it, or subscribing
     *         failed (@p subscription may be set then).
     */
    QString subscribe( natsConnection* connection, natsSubscription** subscription );

private:
    /// The state of the stream to plan from; an error text if it could not
    /// be read.
    QString readStream( StreamState* state );

    /// The replay has caught up with its stream: say so, or end it.
    void markCaughtUp();

    /// nats.c's message callback; @p closure is the ReplaySubscription.
    static void onMessage( natsConnection*, natsSubscription* handle, natsMsg* msg, void* closure );

    const ReplayConfig replay_;
    Outlet& outlet_;
    jsCtx* js_ = nullptr; ///< The connection thread's, until destroyed.
    /// Made from the plan before subscribing; then used by nats.c's
    /// delivery thread only.
    ReplayStep step_;
    bool followLive_ = false; ///< ReplayPlan::followLive; set before subscribing.
    std::mutex mutex_;        ///< Guards caughtUp_, and orders its marker.
    bool caughtUp_ = false;
};

} // namespace nats_monitor

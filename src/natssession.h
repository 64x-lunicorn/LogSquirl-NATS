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
 * @file natssession.h
 * @brief A capture session: one NATS connection, one subscription, one file.
 *
 * The session writes every message it receives as one line (lineformat.h)
 * to a file in a temporary directory "logsquirl-nats-<pid>-XXXXXX"
 * (tempdirs.h), which LogSquirl opens with follow.  Connection events
 * (connected, disconnected, reconnected, errors, stopped) are written into
 * the file as marker lines too, and reported through the signals.
 *
 * THREAD MODEL
 * ────────────
 * The session object lives on the GUI thread.  A writer thread of its own
 * connects (so start() never waits for the network), owns the file and
 * drains the queue of lines in batches, flushing after each.  nats.c calls
 * the message and connection callbacks on its own threads; they only
 * format a line and enqueue it.  Status reaches the session's signals
 * through queued calls, so they are always emitted on the GUI thread.
 *
 * DROPPED MESSAGES
 * ────────────────
 * No message is lost silently.  nats.c drops messages for a slow consumer
 * once the subscription's pending limits are reached; the session's own
 * queue drops new messages once it holds SessionConfig::queueLimit.  Each
 * drop is counted in exactly one marker line,
 *
 *     <time> $NATS-MONITOR - <n> dropped=1234 (slow consumer)
 *     <time> $NATS-MONITOR - <n> dropped=87 (queue full)
 *
 * written at the gap: a slow-consumer marker before the next message
 * nats.c delivers (or when it reports the slow consumer), a queue-full
 * marker before the next message the queue takes.  Since nats.c says only
 * how many it dropped, not where, a slow-consumer marker may come up to
 * the pending messages early.  Drops not yet marked when the session stops
 * get their markers before "stopped".  droppedCount() is the sum.
 *
 * USAGE
 * ─────
 *   NatsSession session( { "nats://localhost:4222", "orders.>" } );
 *   if ( session.start() )               // creates the file, connects in the background
 *       open( session.filePath() );      // LogSquirl follows this file
 *   session.stop();                      // closes the connection, keeps writing nothing more
 *   session.preserveFile();              // the tab keeps the file after the session is gone
 */

#pragma once

#include "lineformat.h"

#include <QObject>
#include <QString>
#include <QTemporaryDir>

#include <memory>

namespace nats_monitor {

/// The server URL the dialog proposes.
inline constexpr const char* kDefaultUrl = "nats://localhost:4222";

/// Defaults for SessionConfig's pending limits: nats.c's own (65,536
/// messages, 64 MiB) drop too early for a capture.
inline constexpr int kDefaultPendingMessages = 1000000;
inline constexpr int kDefaultPendingBytes = 512 * 1024 * 1024;

/// Default for SessionConfig::queueLimit: two seconds at 50k msgs/s.
inline constexpr int kDefaultQueueLimit = 100000;

struct SessionConfig {
    QString url = QString::fromLatin1( kDefaultUrl ); ///< One or more URLs, comma-separated.
    QString subject;                                  ///< May contain the wildcards * and >.
    FormatOptions format;
    /// The subscription's pending limits (natsSubscription_SetPendingLimits):
    /// how many messages, and how many bytes, nats.c holds for the session
    /// before it drops as a slow consumer.  Must be > 0.
    int pendingMessagesLimit = kDefaultPendingMessages;
    int pendingBytesLimit = kDefaultPendingBytes;
    /// How many messages the session's own queue to the writer holds before
    /// it drops new ones (queue full).  Must be > 0.
    int queueLimit = kDefaultQueueLimit;
};

class NatsSession : public QObject {
    Q_OBJECT

public:
    explicit NatsSession( const SessionConfig& config, QObject* parent = nullptr );

    /// Stops the session without emitting any signal; removes the
    /// temporary directory unless preserveFile() was called.
    ~NatsSession() override;

    NatsSession( const NatsSession& ) = delete;
    NatsSession& operator=( const NatsSession& ) = delete;

    /**
     * Create the file and start connecting and subscribing in the
     * background.  No-op (true) if already running.
     *
     * Whether the connection succeeds is reported later: connected(), or
     * errorOccurred() and finished().
     *
     * @return false, after errorOccurred(), if the session could not start
     *         (no subject, the file cannot be created).
     */
    bool start();

    /**
     * Close the connection, write what was received before and a
     * "stopped" marker, and close the file.  Waits for the writer thread,
     * which nats.c lets go within a few seconds at most.  No-op if not
     * running.
     */
    void stop();

    /// Whether the session runs: between start() and stop() or finished().
    bool isRunning() const;

    /// The file the session writes; valid after start().
    QString filePath() const;

    /// Keep the temporary directory when the session is destroyed, for the
    /// tab that shows the file.  The plugin removes it when LogSquirl quits.
    void preserveFile();

    const SessionConfig& config() const
    {
        return config_;
    }

    /// How many messages nats.c has handed to the session since start(),
    /// written or dropped from the session's queue: what a msgs/s is
    /// computed from.  Kept after stop().  Any thread may call it, though
    /// it is meant for the GUI thread.
    quint64 receivedCount() const;

    /// How many messages were dropped since start(), slow consumer and
    /// queue full together: the sum of the dropped= markers written so far
    /// or about to be.  After stop() it equals the sum of all of them.
    quint64 droppedCount() const;

    struct PendingLimits {
        int messages = 0;
        int bytes = 0;
    };

    /// The pending limits nats.c has for the subscription, read back from
    /// it once subscribed; zero before, and after a failed subscribe.
    PendingLimits pendingLimits() const;

Q_SIGNALS:
    /// Connected to @p url and subscribed: after start(), and again after
    /// each reconnect.
    void connected( const QString& url );

    /// A connection event, as written to the file (without the marker
    /// line's fixed fields).
    void statusChanged( const QString& text );

    /// The session could not start, connect or subscribe, or nats.c
    /// reported an error.
    void errorOccurred( const QString& message );

    /// The session ended on its own (it could not connect, or the
    /// connection was closed for good); not emitted by stop().
    void finished();

private:
    struct Worker;

    /// The writer thread of start number @p generation has ended on its own.
    void onWorkerFinished( int generation );

    /// Stop the writer, wait for it and keep its counts.
    void releaseWorker();

    SessionConfig config_;
    QTemporaryDir tempDir_; ///< Declared before worker_: removed after the file is closed.
    QString filePath_;
    int generation_ = 0; ///< Counts start(), to tell a stale finished report from the current.
    std::unique_ptr<Worker> worker_;
    quint64 received_ = 0; ///< The counts of the last worker, once it is gone.
    quint64 dropped_ = 0;
    PendingLimits pendingLimits_;
};

} // namespace nats_monitor

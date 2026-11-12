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
 * @brief A capture session: one NATS connection, its subscriptions, one file.
 *
 * The session writes every message it receives as one line (lineformat.h)
 * to a file, which LogSquirl opens with follow, in a temporary directory
 * "logsquirl-nats-<pid>-XXXXXX" (tempdirs.h) below the root its creator
 * gives it.  Connection events (connected, disconnected, reconnected,
 * errors, stopped) are written into the file as marker lines too, and
 * reported through the signals.  What a session is set up with, its
 * SessionConfig, is in sessionconfig.h.
 *
 * SUBSCRIPTIONS
 * ─────────────
 * A session subscribes to one or more subjects (SessionConfig::subscriptions),
 * each optionally in a queue group, and writes the messages of all of them
 * into its one file, in the order they arrive.  When subjects overlap
 * (orders.> and orders.eu.*), the server delivers a message that matches
 * several once for each subscription, and the session writes each: one line
 * per subscription, no deduplication, so that the file shows what the server
 * sent.  (The dialog warns of overlapping subjects: subjects.h.)  Pending
 * limits, drops and draining are per subscription; the counts are the
 * session's.
 *
 * THREAD MODEL
 * ────────────
 * The session object lives on the GUI thread.  A connection thread of its
 * own connects and subscribes (so start() never waits for the network),
 * waits for the end, and drains and closes.  Everything between a message
 * and the file is the session's capture sink (capturesink.h), whose writer
 * thread owns the file and writes the queued lines in batches, flushing
 * after each, and rotates.  nats.c calls the message and connection
 * callbacks on its own threads; they only format a line and hand it to the
 * sink, never touching the file nor, in a live capture, waiting for the
 * writer.  What the sink reports reaches the session's signals through
 * queued calls, so they are always emitted on the GUI thread.
 *
 * DROPPED MESSAGES
 * ────────────────
 * No message is lost silently.  nats.c drops messages for a slow consumer
 * once a subscription's pending limits are reached; the sink's queue drops
 * new messages once it holds SessionConfig::queueLimit.  Each drop is
 * counted in exactly one marker line,
 *
 *     <time> $NATS-MONITOR - <n> dropped=1234 (slow consumer)
 *     <time> $NATS-MONITOR - <n> dropped=87 (queue full)
 *
 * written at the gap: a slow-consumer marker before the next message
 * nats.c delivers (or when it reports the slow consumer, or the connection
 * breaks), a queue-full marker before the next message the queue takes.
 * Drops not yet marked when the session stops get their markers before
 * "stopped".  droppedCount() is the sum.
 *
 * A slow-consumer marker may land up to the pending-message limit earlier
 * in the file than the actual gap: nats.c reports how many it dropped, not
 * where, so the messages it still held when it dropped, received before
 * the gap, are written after the marker.
 *
 * stop() drains each subscription: nats.c delivers what it holds and what
 * the server sent before the unsubscribe, so nothing that reached the
 * session before stop() is lost uncounted.
 *
 * ROTATION
 * ────────
 * A file grows to SessionConfig::maxFileBytes at most.  When the next line
 * would not fit, the sink's writer closes the file there, at a line
 * boundary, and goes on in the next one: nats_<name>.log, nats_<name>.2.log,
 * nats_<name>.3.log, …, all in the session's temporary directory, and the
 * session reports it with fileRotated().  A line longer than the limit is
 * written whole, alone in its file.  Should the next file not open, the
 * writer keeps writing the current one, reports the error once and tries
 * again once that file has grown by the limit once more.  0 means no limit.
 *
 * REPLAY
 * ──────
 * With SessionConfig::replay set, the session replays a JetStream stream
 * instead of subscribing: what the stream already holds, from the start
 * position, optionally only the messages on a subject.  It reads through an
 * ordered ephemeral consumer with AckNone (nats.c's ordered push consumer),
 * never durable and never acknowledging, so the replay takes no message
 * away from anyone and leaves the stream as it is.  A replayed line has the
 * time the stream stored the message and its stream sequence (lineformat.h),
 * and the reply field "-" (the stream keeps no reply subject).
 *
 * Once the replay has caught up (the consumer has nothing more pending), it
 * either ends, with a "replay complete" marker and completed(), or, with
 * ReplayConfig::followLive, writes "caught up, following live" and goes on
 * with what the stream stores from then on, until stop().
 *
 * The writer, rotation and drop accounting are those of a live capture,
 * except that a full queue makes the delivery wait instead of dropping
 * (the sink's CaptureSink::Mode::WaitForRoom): the
 * stream holds the messages, and JetStream's flow control holds the server
 * back meanwhile, so a replay of any size is written whole at the speed of
 * the writer.  stop() ends a replay at any point: what was queued is
 * written, what nats.c still holds is discarded, uncounted and with no
 * dropped= marker (the stream keeps it), so a stop does not wait for it.
 *
 * "The last N messages" with a filter subject reads the stream from its
 * start and writes the last N that match; without a filter it starts at
 * the stream sequence N before the end.  Where a replay starts, what it
 * skips and when it has caught up are planned from the stream's state
 * before it subscribes (replay.h, planReplay() and ReplayStep).
 *
 * USAGE
 * ─────
 *   SessionConfig config;
 *   config.subscriptions = { { "orders.>", {} }, { "jobs.*", "workers" } };
 *   NatsSession session( config, root ); // its directory goes below root
 *   if ( session.start() )               // creates the file, connects in the background
 *       open( session.filePath() );      // LogSquirl follows this file
 *   session.stop();                      // closes the connection, keeps writing nothing more
 *   session.preserveFile();              // the tab keeps the file after the session is gone
 */

#pragma once

#include "sessionconfig.h"

#include <QObject>
#include <QString>
#include <QTemporaryDir>

#include <memory>

namespace nats_monitor {

class NatsSession : public QObject {
    Q_OBJECT

public:
    /// A session of @p config that writes into a new temporary directory
    /// "logsquirl-nats-<pid>-XXXXXX" below @p tempRoot (tempdirs.h).  Its
    /// creator says where: the plugin's root (Sessions), a test's own.
    NatsSession( const SessionConfig& config, const QString& tempRoot, QObject* parent = nullptr );

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
     *         (its config has a startProblem(), profiles.h, such as no
     *         subject or a blank one; the file cannot be created).
     */
    bool start();

    /**
     * Drain the subscription and close the connection, write what was
     * received before and a "stopped" marker, and close the file.  Waits
     * for the connection thread and the sink's writer: a round trip to the
     * server and the writing of
     * what nats.c held, at most about 15 s should the connection break
     * while draining.  No-op if not running.
     */
    void stop();

    /// Whether the session runs: between start() and stop() or finished().
    bool isRunning() const;

    /// The file the session writes; valid after start().  After a rotation
    /// the new file, once fileRotated() has been emitted for it.
    QString filePath() const;

    /// Keep the temporary directory when the session is destroyed, for the
    /// tab that shows the file.  The plugin removes it when LogSquirl quits.
    void preserveFile();

    const SessionConfig& config() const
    {
        return config_;
    }

    /// How many messages nats.c has handed to the session since start(),
    /// written or dropped from its sink's queue: what a msgs/s is
    /// computed from.  Kept after stop().  Any thread may call it, though
    /// it is meant for the GUI thread.
    ///
    /// receivedCount() and droppedCount() are each read on their own, not
    /// as one consistent snapshot: while messages arrive, one may be a few
    /// messages ahead of the other.  Fine for a msgs/s and a running total;
    /// after stop() both are final.
    quint64 receivedCount() const;

    /// How many messages were dropped since start(), slow consumer and
    /// queue full together: the sum of the dropped= markers written so far
    /// or about to be.  After stop() it equals the sum of all of them.
    quint64 droppedCount() const;

    /// The pending limits nats.c has for each subscription, read back from
    /// the first once subscribed; zero before, and after a failed subscribe.
    PendingLimits pendingLimits() const;

    /// What the accessors above return, taken together.
    struct Counts {
        quint64 received = 0;
        quint64 dropped = 0;
        PendingLimits pendingLimits;
    };

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

    /// The file reached the size limit: the capture goes on in @p path,
    /// the next file of the session (ROTATION).
    void fileRotated( const QString& path );

    /// The session ended on its own (it could not connect, or the
    /// connection was closed for good); not emitted by stop().
    void finished();

    /// A replay that does not follow live has caught up with its stream
    /// and ended: the session no longer runs.  Not emitted by stop().
    void completed();

private:
    struct Worker;

    /// The connection thread of start number @p generation has ended on its own.
    void onWorkerFinished( int generation );

    /// The replay of start number @p generation is complete: its threads have ended.
    void onWorkerCompleted( int generation );

    /// The sink of start number @p generation goes on in @p path.
    void onWorkerRotated( int generation, const QString& path );

    /// Stop the worker, wait for its threads and keep its counts.
    void releaseWorker();

    /// The running worker's counts, or those the last one left.
    Counts counts() const;

    /// With its secret, which start() needs again; the worker clears its
    /// copy once nats.c's options hold it.
    SessionConfig config_;
    QTemporaryDir tempDir_; ///< Declared before worker_: removed after the file is closed.
    QString filePath_;
    int generation_ = 0; ///< Counts start(), to tell a stale finished report from the current.
    std::unique_ptr<Worker> worker_;
    Counts counts_; ///< The counts of the last worker, once it is gone.
};

} // namespace nats_monitor

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
 * TRANSITIONS
 * ───────────
 * Each step of the session's life is one typed Transition (sessionstatus.h),
 * emitted once, in the order of the file's markers:
 *
 *     start() ──► Connecting ──► Running(server) ◄──► Reconnecting
 *                     │               │                   │
 *                     ├───────────────┴─────► Failed(why), Completed
 *                     │
 *   requestStop() ──► Stopping ──► Stopped (or the end the thread had reached)
 *
 * Connecting when start() has opened the file; Running once connected and
 * subscribed (its "connected" marker written), again after a reconnect;
 * Reconnecting when the connection breaks.  The end comes from the
 * connection thread's decision: Failed with the reason it could not start,
 * connect or subscribe, "connection closed", or the write error that the
 * file can no longer be written ("Cannot write to <file>: <why>"); Completed
 * for a replay that caught up; Stopped once a stop has ended it.  A Failed
 * start() emits Failed alone.  An error that does not end the session
 * (nats.c's, a next file that cannot be started) is errorOccurred()
 * instead.  Nobody reads a state out of the texts.
 *
 * Stopping, the one transition without a marker in the file, comes from
 * requestStop() (and stop()) at once, from any state it runs in; after it
 * nothing but the end: a Running or Reconnecting still on its way is not
 * emitted.  The end is Stopped, or what the connection thread had decided
 * before it saw the stop (Failed, Completed), as the file's last marker
 * says.
 *
 * STOPPING
 * ────────
 * requestStop() returns at once: what nats.c is in the middle of (a
 * connect, the round trip after subscribing, a replay's JetStream requests)
 * it cannot interrupt, and the drain and close come after.  The connection
 * thread ends the session in the background, and Stopped follows through
 * the event loop.  stop() does the same and waits for it.  The longest
 * stop, with a server that does not answer, is what is left of the start
 * (nats.c's connect timeout of 2 s for each server of the URL list, then a
 * capture's round trip of 10 s or a replay's three JetStream requests of
 * 5 s each), the drain (kDrainTimeout, 15 s; not connected, up to
 * kCallbackTimeout, 5 s, until nats.c holds nothing more), the close
 * (kCallbackTimeout, 5 s, for nats.c's last callback): 2 s for each server
 * and 30 s for a capture, 35 s for a replay.  Then the writer writes what
 * was still queued, which no timeout bounds.  A server that never answers the connect takes the 2 s
 * for each server alone.
 *
 * A write error ends the session as a stop does: the subscriptions go,
 * nothing further is counted (receivedCount() no longer grows from the
 * error on), and it can be started again once the cause is fixed.
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
 * either ends, with a "replay complete" marker and Completed, or, with
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
 * before it subscribes (replay.h, planReplay() and ReplayStep); the
 * replay's connection path, its nats.c side, is replaysubscription.h.
 *
 * USAGE
 * ─────
 *   SessionConfig config;
 *   config.subscriptions = { { "orders.>", {} }, { "jobs.*", "workers" } };
 *   NatsSession session( config, root ); // its directory goes below root
 *   if ( session.start() )               // creates the file, connects in the background
 *       open( session.filePath() );      // LogSquirl follows this file
 *   session.requestStop();               // Stopping at once; Stopped once it has ended
 *   session.preserveFile();              // the tab keeps the file after the session is gone
 */

#pragma once

#include "capturesink.h"
#include "sessionconfig.h"
#include "sessionstatus.h"

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
    /// @p files opens the capture's files (QFile by default); a test gives
    /// one whose writes fail.
    NatsSession( const SessionConfig& config, const QString& tempRoot, QObject* parent = nullptr,
                 CaptureSink::FileFactory files = {} );

    /// Stops the session without emitting any signal, and waits until it
    /// has ended (stop()): nats.c's callbacks use what the session frees,
    /// and its threads run code of the plugin's library.  Removes the
    /// temporary directory unless preserveFile() was called.
    ~NatsSession() override;

    NatsSession( const NatsSession& ) = delete;
    NatsSession& operator=( const NatsSession& ) = delete;

    /**
     * Create the file and start connecting and subscribing in the
     * background, a Connecting transition.  No-op (true) if already
     * running; false, and no transition, while it is stopping: it starts
     * again once it has ended.
     *
     * Whether the connection succeeds is reported later: Running, or
     * Failed.
     *
     * The password or token (AuthConfig::secret) is held no longer than
     * needed: once started, config() has none, and once nats.c's options
     * hold it the session's copy is wiped (secrets.h); nats.c reconnects
     * with its own.  Started again after stop(), the session has none to
     * log in with: a new session takes it again (Sessions makes one for
     * each start).
     *
     * @return false, after a Failed transition, if the session could not
     *         start (its config has a startProblem(), profiles.h, such as no
     *         subject or a blank one; the file cannot be created).
     */
    bool start();

    /**
     * Ask the session to stop, a Stopping transition, and return at once.
     * In the background the connection thread drains the subscriptions and
     * closes the connection, and the sink writes what was received before
     * and a "stopped" marker and closes the file; then, through the event
     * loop, the transition that ends it (Stopped).  How long that may take
     * is in STOPPING.  No-op if not running or stopping already.
     */
    void requestStop();

    /**
     * Stop as requestStop() does and wait until the session has ended: its
     * threads joined, its file closed, the transition that ends it emitted
     * before this returns.  At most the longest stop (STOPPING), waited for
     * on the calling thread: for a test, and for the plugin's shutdown,
     * which has asked every session to stop first so that it waits for the
     * slowest, not for their sum.  No-op if not running.
     */
    void stop();

    /// Whether the session runs: between start() and the transition that
    /// ends it (Failed, Completed, Stopped), stopping included.
    bool isRunning() const;

    /// Whether it was asked to stop and has not ended yet.
    bool isStopping() const;

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

    /// The pending limits nats.c has for each subscription, read back once
    /// subscribed; zero before, and after a failed subscribe.
    PendingLimits pendingLimits() const;

    /// What the accessors above return, taken together.
    struct Counts {
        quint64 received = 0;
        quint64 dropped = 0;
        PendingLimits pendingLimits;
    };

Q_SIGNALS:
    /// The session went a step on (TRANSITIONS): once for each step.
    void transitioned( const Transition& transition );

    /// nats.c reported an error, or the next file could not be started, and
    /// the session runs on.  What ends it, a write error among them, is a
    /// Failed transition instead.
    void errorOccurred( const QString& message );

    /// The file reached the size limit: the capture goes on in @p path,
    /// the next file of the session (ROTATION).
    void fileRotated( const QString& path );

private:
    struct Worker;

    /// The sink of start number @p generation wrote the marker of
    /// @p transition: emit it, unless the session ended meanwhile.
    void onWorkerTransition( int generation, const Transition& transition );

    /// The connection thread of start number @p generation has ended, on its
    /// own or stopped: its threads have ended.
    void onWorkerEnded( int generation );

    /// The sink of start number @p generation goes on in @p path.
    void onWorkerRotated( int generation, const QString& path );

    /// Stop the worker, wait for its threads, keep its counts, and emit the
    /// transition it ended with.
    void endWorker();

    /// The running worker's counts, or those the last one left.
    Counts counts() const;

    /// Its secret only until start() hands it to the worker, which wipes it
    /// once nats.c's options hold it (secrets.h).
    SessionConfig config_;
    const CaptureSink::FileFactory files_; ///< Empty: QFile.
    QTemporaryDir tempDir_; ///< Declared before worker_: removed after the file is closed.
    QString filePath_;
    int generation_ = 0; ///< Counts start(), to tell a stale finished report from the current.
    std::unique_ptr<Worker> worker_;
    bool stopping_ = false; ///< Asked to stop; the worker has not ended yet.
    Counts counts_;         ///< The counts of the last worker, once it is gone.
};

} // namespace nats_monitor

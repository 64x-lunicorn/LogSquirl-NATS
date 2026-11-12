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
 * @file capturesink.h
 * @brief A capture sink: everything between a received message and the
 *        capture's files.
 *
 * The sink takes message lines and marker lines from any thread and writes
 * them, in the order it took them, into the numbered files of a capture:
 * <stem>.log, <stem>.2.log, <stem>.3.log, …  It owns the bounded queue, the
 * accounting of drops, the back-pressure of a replay, the writer thread,
 * the rotation at the size limit and the end of the capture.  It knows
 * nothing of NATS: the session (natssession.h) feeds it from nats.c's
 * callbacks and forwards what it reports.
 *
 * THE QUEUE
 * ─────────
 * It holds up to Config::queueLimit message lines; marker lines always fit.
 * In Mode::Drop (a live subscription) a message that finds it full is
 * dropped and counted, and the next message taken is preceded by its
 * marker,
 *
 *     <time> $NATS-MONITOR - <n> dropped=87 (queue full)
 *
 * so message() never waits for the writer.  In Mode::WaitForRoom (a replay)
 * message() waits until the writer has taken the queue instead, and drops
 * nothing.  Drops counted elsewhere (nats.c's slow consumer) are marked
 * with dropped(), at the gap.  Every drop is in exactly one dropped= marker;
 * those not marked yet when the capture ends are written before its last
 * marker.
 *
 * THE WRITER
 * ──────────
 * start() runs the writer thread: it waits until lines are queued, swaps
 * the whole queue out (never holding the mutex while writing), writes the
 * batch and flushes once.  A sink not started is written on the caller's
 * thread with writeQueued() and finish(): that is how the tests drive it,
 * deterministically.
 *
 * ROTATION
 * ────────
 * Before a line that would take the file over Config::maxFileBytes the
 * writer opens the next file (NewOnly: an earlier file is never truncated),
 * closes the full one there, at a line boundary, and reports
 * Event::Rotated.  A line longer than the limit is written whole, alone in
 * its file.  Should the next file not open, the writer keeps writing the
 * current one, reports Event::FileError, and tries again once that file has
 * grown by the limit once more.  0 means no limit.  A write or flush that
 * fails is reported once, as Event::FileError; nothing more is written.
 *
 * EVENTS
 * ──────
 * A marker line may carry an event, which the writer reports as it writes
 * the line (also after a write failure), in the order of the lines: a
 * report never comes before its line is handed to the file.  The kinds
 * (Event::Kind) are typed so that the session can map each to a transition
 * or a signal without reading the text.
 * The reporter is called on the writer thread (or the caller's, not
 * started), never under the sink's mutex.
 *
 * LIFETIME
 * ────────
 * finish() ends the capture: the drops not marked yet, what is queued, the
 * last marker, the file closed and the writer joined.  From then on the
 * sink takes nothing more and touches no file, so a callback that nats.c
 * still makes is harmless: the sink may be leaked with what the callbacks
 * use, as the session does when nats.c does not confirm their end.
 */

#pragma once

#include "sessionconfig.h"

#include <QByteArray>
#include <QFile>
#include <QString>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace nats_monitor {

class CaptureSink {
public:
    /// What a full queue does with a message.
    enum class Mode {
        Drop,        ///< Drop and count it: a live subscription.
        WaitForRoom, ///< Wait until the writer took the queue: a replay.
    };

    /// What the sink reports, once the line that goes with it is written.
    struct Event {
        enum class Kind {
            Connected,    ///< A marker; the text is the server URL.
            Reconnected,  ///< A marker; the text is the server URL.
            Disconnected, ///< A marker of the connection broken; the text is what it says.
            /// A marker the session reports nothing more of: a warning,
            /// caught up, the end of the capture or why it failed (the
            /// session's own transitions say those); the text is what it says.
            Status,
            Error,   ///< A marker of an error the capture runs on after; the text is what it says.
            Rotated, ///< The capture goes on in another file; the text is its path.
            FileError, ///< A file could not be written or started; the text says why.
        };
        Kind kind = Kind::Status;
        QString text;

        bool operator==( const Event& other ) const
        {
            return kind == other.kind && text == other.text;
        }
    };

    using Reporter = std::function<void( const Event& )>;

    /// Opens a file of the capture by its path (not yet opened); a test
    /// gives one that fails.
    using FileFactory = std::function<std::unique_ptr<QFile>( const QString& path )>;

    struct Config {
        QString fileStem;                    ///< The first file's path without ".log".
        qint64 maxFileBytes = 0;             ///< Of each file; 0 for no limit.
        int queueLimit = kDefaultQueueLimit; ///< Message lines the queue holds.
        Mode mode = Mode::Drop;
    };

    /// A sink of @p config reporting to @p reporter; @p files opens its
    /// files (QFile by default).
    CaptureSink( const Config& config, Reporter reporter, FileFactory files = {} );

    /// finish()es the capture, without a last marker, if it was not.
    ~CaptureSink();

    CaptureSink( const CaptureSink& ) = delete;
    CaptureSink& operator=( const CaptureSink& ) = delete;

    /// Open the last file there is, or the first: a capture started again
    /// goes on in it, with what it holds counted against the limit.  On
    /// the creator's thread, before start().
    bool open();

    /// The file being written; after a rotation the new one.  Before
    /// start(), or once finished.
    QString fileName() const;

    /// Why open() failed.
    QString errorString() const;

    /// Run the writer thread.  Not started, the caller writes with
    /// writeQueued() and finish().
    void start();

    // ── Any thread ──────────────────────────────────────────────────────

    /**
     * Take the message line @p line (without terminator): queue it,
     * preceded by the marker of the queue-full drops not marked yet, or
     * drop and count it if the queue is full (Mode::Drop), or wait for room
     * (Mode::WaitForRoom).  Counted as received if queued or dropped.
     *
     * @return false if it was discarded instead, uncounted: after
     *         discardMessages() or finish().
     */
    bool message( QByteArray line );

    /// Queue the marker line saying @p text; the writer then reports
    /// @p kind with @p eventText, or with @p text if that is empty.
    void marker( const QString& text, Event::Kind kind, const QString& eventText = {} );

    /// Count @p count messages dropped elsewhere, for @p reason, and queue
    /// their marker: "dropped=<count> (<reason>)".
    void dropped( quint64 count, const char* reason );

    /// Discard every message from now on, uncounted and unmarked, and wake
    /// a message() that waits for room: a replay being stopped, whose stream
    /// keeps them.  Marker lines are still written.
    void discardMessages();

    /// Whether discardMessages() was called.
    bool discarding() const
    {
        return discarding_.load();
    }

    /// Messages taken since the sink was made, queued or dropped.
    quint64 received() const
    {
        return received_.load();
    }

    /// Drops counted since the sink was made: marked, or to be marked.
    quint64 dropped() const
    {
        return dropped_.load();
    }

    /// The callers of message() waiting for room now.
    int waitingForRoom() const;

    // ── The thread that started the sink ──────────────────────────────

    /// Write what is queued, on this thread; only when not start()ed.
    void writeQueued();

    /**
     * End the capture: queue the marker of the queue-full drops not marked
     * yet and then, unless @p text is empty, the last marker saying @p text
     * (reported as @p kind); write all of it, close the file and join the
     * writer.  The sink takes nothing more.  No-op once finished.
     */
    void finish( const QString& text = {}, Event::Kind kind = Event::Kind::Status );

private:
    class RotatingFile;

    struct Item {
        QByteArray line; ///< With its terminator.
        bool reports = false;
        Event event;
    };

    /// Queue @p line (without terminator), reporting nothing; under the mutex.
    void enqueueLocked( QByteArray line );

    /// Queue the marker line @p line (without terminator), reported as
    /// @p event; under the mutex.
    void enqueueLocked( QByteArray line, const Event& event );

    /// Queue the marker of the queue-full drops not marked yet; under the mutex.
    void markQueueFullLocked();

    /// Take what is queued, under the mutex.
    std::vector<Item> takeLocked();

    /// The writer thread's loop.
    void run();

    /// Write and flush a batch, and report its events.
    void write( const std::vector<Item>& batch );

    /// Go on in the next file if a line of @p lineBytes would not fit.
    void rotateBefore( qint64 lineBytes );

    /// The file cannot be written: say so once.
    void fail();

    void report( const Event& event );

    const Config config_;
    const Reporter reporter_;
    std::unique_ptr<RotatingFile> file_; ///< The writer's.
    bool writeFailed_ = false;           ///< The writer's.

    mutable std::mutex mutex_;
    std::condition_variable wake_; ///< Lines queued, or finishing.
    std::condition_variable room_; ///< The writer took the queue, or no waiting any more.
    std::vector<Item> queue_;
    int queuedMessages_ = 0;        ///< Message lines in the queue.
    quint64 queueFullUnmarked_ = 0; ///< Dropped from a full queue, not marked yet.
    int waiting_ = 0;               ///< Callers of message() waiting for room.
    bool finishing_ = false;        ///< finish() was called: take nothing more.
    std::atomic<bool> discarding_{ false };
    std::atomic<quint64> received_{ 0 };
    std::atomic<quint64> dropped_{ 0 };
    std::thread thread_;
};

} // namespace nats_monitor

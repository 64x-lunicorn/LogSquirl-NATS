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
 * to a file in a temporary directory "logsquirl-nats-<pid>-XXXXXX"
 * (tempdirs.h), which LogSquirl opens with follow.  Connection events
 * (connected, disconnected, reconnected, errors, stopped) are written into
 * the file as marker lines too, and reported through the signals.
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
 * once a subscription's pending limits are reached; the session's own
 * queue drops new messages once it holds SessionConfig::queueLimit.  Each
 * drop is counted in exactly one marker line,
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
 * would not fit, the writer closes the file there, at a line boundary, and
 * goes on in the next one: nats_<name>.log, nats_<name>.2.log,
 * nats_<name>.3.log, …, all in the session's temporary directory, and
 * reports it with fileRotated().  A line longer than the limit is written
 * whole, alone in its file.  Should the next file not open, the writer
 * keeps writing the current one, reports the error once and tries again
 * once that file has grown by the limit once more.  0 means no limit.
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
 * except that a full queue makes the delivery wait instead of dropping: the
 * stream holds the messages, and JetStream's flow control holds the server
 * back meanwhile, so a replay of any size is written whole at the speed of
 * the writer.  stop() ends a replay at any point, as it does a capture.
 *
 * "The last N messages" with a filter subject reads the stream from its
 * start and writes the last N that match; without a filter it starts at
 * the stream sequence N before the end.
 *
 * USAGE
 * ─────
 *   SessionConfig config;
 *   config.subscriptions = { { "orders.>", {} }, { "jobs.*", "workers" } };
 *   NatsSession session( config );
 *   if ( session.start() )               // creates the file, connects in the background
 *       open( session.filePath() );      // LogSquirl follows this file
 *   session.stop();                      // closes the connection, keeps writing nothing more
 *   session.preserveFile();              // the tab keeps the file after the session is gone
 */

#pragma once

#include "lineformat.h"

#include <QList>
#include <QObject>
#include <QString>
#include <QTemporaryDir>

#include <memory>

namespace nats_monitor {

/// The server URL the dialog proposes.
inline constexpr const char* kDefaultUrl = "nats://localhost:4222";

/// A subscription's pending limits (natsSubscription_SetPendingLimits): how
/// many messages, and how many bytes, nats.c holds for the session before
/// it drops as a slow consumer.
struct PendingLimits {
    int messages = 0;
    int bytes = 0;
};

/// Default for SessionConfig::pendingLimits: nats.c's own (65,536
/// messages, 64 MiB) drop too early for a capture.
inline constexpr PendingLimits kDefaultPendingLimits{ 1000000, 512 * 1024 * 1024 };

/// Default for SessionConfig::queueLimit: two seconds at 50k msgs/s.
inline constexpr int kDefaultQueueLimit = 100000;

/// Bytes in a MiB, the unit the size limit is set in.
inline constexpr qint64 kBytesPerMiB = 1024 * 1024;

/// The size limit in MiB the dialog proposes, unless the user chose another.
inline constexpr int kDefaultMaxFileMiB = 256;

/// The default inactive threshold of a replay's consumer, in milliseconds:
/// the server deletes it this long after its reader went away.
inline constexpr int kDefaultInactiveThresholdMs = 5000;

/// How a session proves who it is to the server.
enum class AuthMethod {
    None,         ///< No credentials.
    UserPassword, ///< AuthConfig::user and the password in AuthConfig::secret.
    Token,        ///< The token in AuthConfig::secret.
    Credentials,  ///< A .creds file (user JWT and NKey seed) in AuthConfig::file.
    NKey,         ///< A file with an NKey seed (SU…) in AuthConfig::file.
};

struct AuthConfig {
    AuthMethod method = AuthMethod::None;
    QString user;
    QString secret; ///< The password or token: asked for at Start, never saved.
    QString file;   ///< The .creds or NKey seed file.
};

struct TlsConfig {
    /// Connect with TLS; a tls:// URL does so too.
    bool enabled = false;
    QString caFile;     ///< The CA(s) the server's certificate must be signed by (PEM).
    QString certFile;   ///< A client certificate (PEM); with keyFile.
    QString keyFile;    ///< Its private key (PEM).
    QString serverName; ///< The name the server's certificate must be for; empty: the URL's host.
    bool skipVerify = false; ///< Accept any server certificate: insecure.
};

/// A subject the session subscribes to.
struct Subscription {
    QString subject; ///< May contain the wildcards * and >.
    /// The queue group to subscribe in; empty for none, the default.  The
    /// server gives each message of a queue group to one member only, so a
    /// session in a group takes messages away from the other members.
    QString queueGroup;
};

/// @p subscriptions as the marker lines and the dialog name them:
/// "orders.>, jobs.* (queue workers)".
QString describeSubscriptions( const QList<Subscription>& subscriptions );

/// Where a replay starts in its stream (REPLAY).
enum class ReplayStart {
    All,      ///< At the stream's first message.
    Last,     ///< At the last ReplayConfig::lastMessages messages (that match the filter).
    Sequence, ///< At the stream sequence ReplayConfig::sequence.
    Time,     ///< At the first message stored at or after ReplayConfig::timeUs.
};

/// What a session replays from a JetStream stream (REPLAY).
struct ReplayConfig {
    /// The stream to replay; empty: no replay, the session subscribes.
    QString stream;
    /// Replay only the messages on this subject (wildcards allowed); empty
    /// for all the stream's messages.
    QString filterSubject;
    ReplayStart start = ReplayStart::All;
    quint64 lastMessages = 100; ///< For ReplayStart::Last; at least 1.
    quint64 sequence = 1;       ///< For ReplayStart::Sequence; at least 1.
    qint64 timeUs = 0;          ///< For ReplayStart::Time: microseconds since the epoch (UTC).
    /// Once caught up, go on with the messages the stream stores from then
    /// on, instead of ending.
    bool followLive = false;
    /// How long the server keeps the replay's consumer once nobody reads
    /// it: after a crash, or a connection lost for good.  0 leaves it to
    /// the server.
    int inactiveThresholdMs = kDefaultInactiveThresholdMs;

    bool enabled() const
    {
        return !stream.isEmpty();
    }
};

struct SessionConfig {
    /// One or more URLs, comma-separated: the seeds of a cluster.
    QString url = QString::fromLatin1( kDefaultUrl );
    /// What the session subscribes to: at least one subject, unless it
    /// replays.
    QList<Subscription> subscriptions;
    /// The stream the session replays instead, if enabled(); the
    /// subscriptions are not used then.
    ReplayConfig replay;
    AuthConfig auth;
    TlsConfig tls;
    FormatOptions format;
    /// The subscription's pending limits; both must be > 0.
    PendingLimits pendingLimits = kDefaultPendingLimits;
    /// How many messages the session's own queue to the writer holds before
    /// it drops new ones (queue full).  Must be > 0.
    int queueLimit = kDefaultQueueLimit;
    /// The size at which the capture goes on in a new file (ROTATION); 0
    /// for no limit.
    qint64 maxFileBytes = kDefaultMaxFileMiB * kBytesPerMiB;
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
     *         (no subject or a blank one, the file cannot be created).
     */
    bool start();

    /**
     * Drain the subscription and close the connection, write what was
     * received before and a "stopped" marker, and close the file.  Waits
     * for the writer thread: a round trip to the server and the writing of
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
    /// written or dropped from the session's queue: what a msgs/s is
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

    /// The writer thread of start number @p generation has ended on its own.
    void onWorkerFinished( int generation );

    /// The replay of start number @p generation is complete: its writer has ended.
    void onWorkerCompleted( int generation );

    /// The writer of start number @p generation goes on in @p path.
    void onWorkerRotated( int generation, const QString& path );

    /// Stop the writer, wait for it and keep its counts.
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

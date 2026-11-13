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
 * @file sessionconfig.h
 * @brief What a session is set up with: the server, how it logs in, what it
 *        subscribes to or replays, its limits, and their defaults.
 *
 * Plain values, apart from the session (natssession.h) that runs them: the
 * authentication methods, the subjects, the profiles, the replay, the
 * connection options and the dialog need these and not the session.  What
 * each setting does to a running session is told in natssession.h
 * (SUBSCRIPTIONS, DROPPED MESSAGES, ROTATION, REPLAY).
 */

#pragma once

#include "lineformat.h"

#include <QList>
#include <QString>

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

/// Where a replay starts in its stream (natssession.h, REPLAY).
enum class ReplayStart {
    All,      ///< At the stream's first message.
    Last,     ///< At the last ReplayConfig::lastMessages messages (that match the filter).
    Sequence, ///< At the stream sequence ReplayConfig::sequence.
    Time,     ///< At the first message stored at or after ReplayConfig::timeUs.
};

/// What a session replays from a JetStream stream (natssession.h, REPLAY).
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
    /// The sequence, and the time, as they were given when they are no
    /// number of their field (a word, a sequence past the largest): the
    /// start then cannot be read, and replayProblem() (replay.h) refuses it
    /// rather than start somewhere else.  Empty if read; saved as it was
    /// given, so writing nats.ini again does not turn it into one.  Set by
    /// readStartSequence() and readStartTime().
    QString unreadSequence;
    QString unreadTime;
    /// The number of last messages as it was given when it is no number of
    /// them (a word, one past kMaxLastMessages): replayProblem() refuses it
    /// rather than replay another number.  Empty if read; saved as it was
    /// given, so writing nats.ini again does not turn it into one.  Set by
    /// readLastMessages().
    QString unreadLastMessages;
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
    /// The size at which the capture goes on in a new file (natssession.h,
    /// ROTATION); 0 for no limit.
    qint64 maxFileBytes = kDefaultMaxFileMiB * kBytesPerMiB;
};

} // namespace nats_monitor

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
 * @file profiles.h
 * @brief Connection profiles: a connection's settings saved under a name.
 *
 * A profile is a saved connection: the server URLs, the authentication
 * method with its user name and file, the TLS settings, the subjects and
 * the limits of its capture, or instead of the subjects the JetStream
 * stream it replays.  Never a password or a token: the sidebar asks
 * for those at Start, or they are in a .creds file.  Each profile is a tile
 * in the sidebar (sidebarwidget.h), started from there.
 *
 * PROFILE STORE
 * ─────────────
 * Profiles are saved in nats.ini in the plugin's configuration directory
 * (get_config_dir), as the array "profiles", in the order they were first
 * saved.  A ProfileStore opened on that file is its one owner: the dialog
 * and the sidebar list, find, save and delete the profiles through it, and
 * it remembers the defaults a new connection proposes (the size limit saved
 * last).  When it cannot save or delete, it says why, in words for the
 * user.  It reads the file on each call, so two stores on one file agree.
 *
 * The files of earlier versions load: a profile saved before its capture
 * settings or its replay were gets their defaults.  Tested on temporary
 * files (profiles_test.cpp), and through the dialog and the sidebar
 * (plugin_test.cpp).
 */

#pragma once

#include "sessionconfig.h"

#include <QList>
#include <QObject>
#include <QString>

#include <functional>
#include <optional>

namespace nats_monitor {

/// The largest cut length of binary payloads a profile takes: 1 MiB.
inline constexpr int kMaxBinaryBytesLimit = 1024 * 1024;

/// The largest size limit a profile takes: 1 TiB.
inline constexpr int kMaxFileMiBLimit = 1024 * 1024;

struct ConnectionProfile {
    QString name;
    QString url;
    AuthConfig auth; ///< Its secret is never saved, and empty when loaded.
    TlsConfig tls;
    /// The subjects, one a line, "<subject> [<queue group>]" (subjects.h).
    QString subjects;
    int maxBinaryBytes = kDefaultMaxBinaryBytes;
    PendingLimits pendingLimits = kDefaultPendingLimits;
    int maxFileMiB = kDefaultMaxFileMiB; ///< 0 for no limit.
    /// The stream the connection replays instead of capturing the subjects,
    /// if replay.enabled().
    ReplayConfig replay;

    /// The subscriptions of subjects; those before an error if a line is wrong.
    QList<Subscription> subscriptions() const;

    /// Whether the connection uses TLS: enabled, or a tls:// URL.
    bool usesTls() const;
};

/// What is missing for @p profile to start a capture, in words for the
/// user; empty if nothing.  The secret is not checked: it is asked for at
/// Start.
QString startProblem( const ConnectionProfile& profile );

/// The session configuration of @p profile, with its auth.secret as is.
SessionConfig sessionConfig( const ConnectionProfile& profile );

/// The saved connections of one settings file, and the defaults it
/// remembers; the only code that reads or writes the file.
class ProfileStore : public QObject {
    Q_OBJECT

public:
    /// Whether the connection @p name runs: it is not deleted then.
    using IsRunning = std::function<bool( const QString& name )>;

    /// The store of @p settingsFile, which need not exist yet; empty, the
    /// store has no profile and saves none (no host to say where).
    /// @p isRunning tells which connections run; none if it is empty.
    explicit ProfileStore( const QString& settingsFile, IsRunning isRunning = {},
                           QObject* parent = nullptr );

    /// The file the store reads and writes.
    QString settingsFile() const;

    /// The saved profiles, in the order they were first saved.
    QList<ConnectionProfile> list() const;

    /// The profile named @p name; empty if there is none.
    std::optional<ConnectionProfile> find( const QString& name ) const;

    /// The settings a new connection starts with: the defaults, and the
    /// size limit saved last.
    ConnectionProfile newProfile() const;

    /// Save @p profile under its name, replacing the profile of that name
    /// where it is in the list, or after the others, and remember its size
    /// limit for newProfile().  Never its auth.secret.  Empty if it was
    /// saved, else why not.
    QString upsert( const ConnectionProfile& profile );

    /// Delete the profile named @p name, unless it runs.  Empty if it was
    /// deleted, else why not.
    QString remove( const QString& name );

Q_SIGNALS:
    /// A profile was saved or deleted.
    void changed();

private:
    /// Write @p profiles, replacing those saved, and @p maxFileMiB as the
    /// size limit to remember if set; whether the file was written.
    bool write( const QList<ConnectionProfile>& profiles, std::optional<int> maxFileMiB );

    QString settingsFile_;
    IsRunning isRunning_;
};

} // namespace nats_monitor

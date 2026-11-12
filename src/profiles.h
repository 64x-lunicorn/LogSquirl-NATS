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
 * Profiles are saved in nats.ini in the plugin's configuration directory
 * (get_config_dir), as the array "profiles", in the order they were first
 * saved.  Tested through the dialog (plugin_test.cpp).
 */

#pragma once

#include "sessionconfig.h"

#include <QList>
#include <QString>

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

/// The profiles saved in the settings file @p settingsFile.
QList<ConnectionProfile> loadProfiles( const QString& settingsFile );

/// The profile named @p name in @p settingsFile; empty if there is none.
std::optional<ConnectionProfile> findProfile( const QString& settingsFile, const QString& name );

/// What is missing for @p profile to start a capture, in words for the
/// user; empty if nothing.  The secret is not checked: it is asked for at
/// Start.
QString startProblem( const ConnectionProfile& profile );

/// The session configuration of @p profile, with its auth.secret as is.
SessionConfig sessionConfig( const ConnectionProfile& profile );

/// Save @p profiles to @p settingsFile, replacing those saved before;
/// false if the file could not be written.
bool saveProfiles( const QString& settingsFile, const QList<ConnectionProfile>& profiles );

} // namespace nats_monitor

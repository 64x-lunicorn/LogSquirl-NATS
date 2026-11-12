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
 * A profile holds the server URLs, the authentication method with its user
 * name and file, and the TLS settings.  Never a password or a token: the
 * dialog asks for those at Start, or they are in a .creds file.
 *
 * Profiles are saved in nats.ini in the plugin's configuration directory
 * (get_config_dir), as the array "profiles", in the order they were first
 * saved.  Tested through the dialog (plugin_test.cpp).
 */

#pragma once

#include "natssession.h"

#include <QList>
#include <QString>

namespace nats_monitor {

struct ConnectionProfile {
    QString name;
    QString url;
    AuthConfig auth; ///< Its secret is never saved, and empty when loaded.
    TlsConfig tls;
};

/// The profiles saved in the settings file @p settingsFile.
QList<ConnectionProfile> loadProfiles( const QString& settingsFile );

/// Save @p profiles to @p settingsFile, replacing those saved before;
/// false if the file could not be written.
bool saveProfiles( const QString& settingsFile, const QList<ConnectionProfile>& profiles );

} // namespace nats_monitor

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
 * @file natsoptions.h
 * @brief A session's connection settings as nats.c options, and why a
 *        connection failed in words the user can act on.
 *
 * Part of NatsSession (natssession.cpp) and of listStreams() (replay.cpp),
 * apart because it is all about nats.c's options: the server URLs, the
 * authentication method and TLS of a SessionConfig, and connecting with
 * them.  Tested through the session (auth_test.cpp).
 */

#pragma once

// First: on Windows nats.h includes winsock2.h, which must come before any
// windows.h.
#include <nats.h>

#include "sessionconfig.h"

#include <QString>
#include <QStringList>

namespace nats_monitor {

/// The URLs of @p url: comma-separated, each trimmed, empty ones left out.
QStringList serverUrls( const QString& url );

/**
 * @p url (one or more, comma-separated) as it may be shown or written: the
 * password of each URL's user information replaced by "***", and a user
 * information without a password (which may be a token) as a whole.
 * nats://bob:s3cret@host:4222 becomes nats://bob:***@host:4222.
 */
QString redactUrls( const QString& url );

/// Whether a connection to @p url uses TLS: @p tlsEnabled, or a tls:// URL.
/// The TLS settings apply then, and only then.
bool usesTls( const QString& url, bool tlsEnabled );

/**
 * Set the servers, the authentication and TLS of @p config on @p options.
 *
 * Files named in the configuration are checked here, so a missing or
 * unusable one is reported by its name rather than as a failed handshake.
 *
 * @return empty, or why the options could not be set (for the user).
 */
QString configureConnection( natsOptions* options, const SessionConfig& config );

/**
 * Why connecting with @p config failed, for the user: what the server or
 * TLS refused and what to change, followed by nats.c's own @p detail in
 * parentheses.  Just @p detail when there is nothing to add.
 */
QString explainConnectFailure( const SessionConfig& config, natsStatus status,
                               const QString& detail );

/// nats.c's text for @p status, followed by this thread's last nats.c
/// error in parentheses if there is one.
QString describeStatus( natsStatus status );

/**
 * Set up @p options for @p config (configureConnection()) and connect with
 * them as @p connection: the steps a session and a listing of the streams
 * share.  Any other option (callbacks, a timeout) is the caller's to set
 * on @p options.
 *
 * @return empty, or why it failed, for the user: "connection to <URLs>
 *         failed: …", the URLs without passwords.
 */
QString connectTo( natsOptions* options, const SessionConfig& config, natsConnection** connection );

} // namespace nats_monitor

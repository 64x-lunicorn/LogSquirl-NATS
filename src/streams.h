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
 * @file streams.h
 * @brief The JetStream streams of a server, to choose the one to replay.
 *
 * Apart from planning a replay (replay.h), which is pure: this connects.
 */

#pragma once

#include "sessionconfig.h"

#include <QString>
#include <QStringList>

namespace nats_monitor {

/**
 * The names of the JetStream streams on the server of @p config, sorted
 * ($JS.API.STREAM.NAMES), to choose the one to replay.  Connects with the
 * config's URL, auth and TLS, and waits for the answer: call it off the
 * GUI thread.  It takes at most 2 s for each server URL to connect to
 * (one after the other, should several not answer) and 2 s for each
 * answer of the server.
 *
 * @return the names; empty, with @p error set (for the user), if they
 *         could not be listed.  An empty list without error: no stream.
 */
QStringList listStreams( const SessionConfig& config, QString* error );

} // namespace nats_monitor

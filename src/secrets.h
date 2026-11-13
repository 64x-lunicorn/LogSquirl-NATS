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
 * @file secrets.h
 * @brief Wiping a secret from memory once nats.c has it.
 *
 * A password, a token or an NKey seed is held no longer than needed: the
 * copies made to hand it to nats.c (natsoptions.cpp), the seed decoded to
 * derive its public key (nkey.cpp) and the session's own copy
 * (natssession.cpp) are wiped once used.  nats.c keeps its own copy, for
 * reconnecting.  Tested in secrets_test.cpp; that a session keeps no copy,
 * in auth_test.cpp.
 */

#pragma once

#include <QByteArray>
#include <QString>

namespace nats_monitor {

/**
 * Overwrite what @p bytes holds with zeros, where the compiler cannot leave
 * it out, and empty it.  Only its own data: if another QByteArray shares
 * it (Qt's implicit sharing), that one keeps it, and @p bytes just lets go.
 */
void wipe( QByteArray& bytes );

/// The same for @p text.
void wipe( QString& text );

} // namespace nats_monitor

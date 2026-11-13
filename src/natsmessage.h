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
 * @file natsmessage.h
 * @brief A nats.c message as the formatter takes it (lineformat.h).
 *
 * Shared by a live capture (natssession.cpp) and a replay
 * (replaysubscription.cpp).  Tested through the session.
 */

#pragma once

// First: on Windows nats.h includes winsock2.h, which must come before any
// windows.h.
#include <nats.h>

#include "lineformat.h"

namespace nats_monitor {

/// @p msg as the formatter takes it, received now, its headers sorted by
/// key.  Subject, reply and payload are views into @p msg, valid while it
/// is.
Message toMessage( natsMsg* msg );

} // namespace nats_monitor

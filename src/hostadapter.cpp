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
 * @file hostadapter.cpp
 * @brief Implementation of what the host adapters share.
 */

#include "hostadapter.h"

#include "logsquirl_plugin_api.h"

namespace nats_monitor {

void logFailure( HostAdapter& host, const char* what, const char* reason ) noexcept
{
    try {
        host.log( LOGSQUIRL_LOG_ERROR, QStringLiteral( "NATS Monitor plugin: %1 failed: %2" )
                                           .arg( what, QString::fromUtf8( reason ) ) );
    } catch ( ... ) {
        // Nothing left to report it with.
    }
}

} // namespace nats_monitor

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
 * @file sessionconfig.cpp
 * @brief Implementation of what is said of a session's configuration.
 */

#include "sessionconfig.h"

#include <QStringList>

namespace nats_monitor {

QString describeSubscriptions( const QList<Subscription>& subscriptions )
{
    QStringList names;
    for ( const auto& subscription : subscriptions ) {
        names << ( subscription.queueGroup.isEmpty()
                       ? subscription.subject
                       : QStringLiteral( "%1 (queue %2)" )
                             .arg( subscription.subject, subscription.queueGroup ) );
    }
    return names.join( QStringLiteral( ", " ) );
}

} // namespace nats_monitor

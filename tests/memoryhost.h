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
 * @file memoryhost.h
 * @brief An in-memory host adapter (hostadapter.h), for tests.
 *
 * A MemoryHost records what a module that reaches the host through a host
 * adapter (Sessions) opened, notified and logged, for the test to inspect.
 * It needs no plugin initialised and no host API table: the plugin-level
 * tests, which drive the C entry points, use FakeHost (fakehost.h).
 */

#pragma once

#include "hostadapter.h"
#include "logsquirl_plugin_api.h" // the log levels

#include <QList>
#include <QString>
#include <QStringList>

namespace nats_test {

class MemoryHost : public nats_monitor::HostAdapter {
public:
    /** A message logged, at its level (LOGSQUIRL_LOG_*). */
    struct Log {
        int level;
        QString message;
    };

    void openFollowed( const QString& path ) override
    {
        followedTabs << path;
    }

    void notify( const QString& message ) override
    {
        notifications << message;
    }

    void log( int level, const QString& message ) override
    {
        logs.append( { level, message } );
    }

    /** The messages logged at @p level, in order. */
    QStringList logged( int level ) const
    {
        QStringList messages;
        for ( const auto& log : logs ) {
            if ( log.level == level ) {
                messages << log.message;
            }
        }
        return messages;
    }

    QStringList followedTabs; ///< The files opened in a followed tab, in order.
    QStringList notifications;
    QList<Log> logs;
};

} // namespace nats_test

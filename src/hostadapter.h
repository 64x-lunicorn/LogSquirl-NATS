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
 * @file hostadapter.h
 * @brief What a module needs of LogSquirl, the host: open a file in a
 *        followed tab, show a notification, log.
 *
 * Sessions reaches the host through a HostAdapter it is given, never
 * through the plugin's global state.  The plugin's adapter (pluginHost(),
 * plugin.h) calls the LogSquirl plugin API, and does nothing before init
 * or after shutdown; a test gives an in-memory one that records what was
 * opened, notified and logged (tests/memoryhost.h), with no plugin
 * initialised.
 *
 * Every call comes from the GUI thread.  The host decodes the strings as
 * UTF-8.
 */

#pragma once

#include <QString>

#include <exception>

namespace nats_monitor {

class HostAdapter {
public:
    virtual ~HostAdapter() = default;

    /// Open @p path in a new tab that follows the file as it grows.
    virtual void openFollowed( const QString& path ) = 0;

    /// Show @p message in a host notification.
    virtual void notify( const QString& message ) = 0;

    /// Write @p message to the host log at @p level (LOGSQUIRL_LOG_*,
    /// logsquirl_plugin_api.h).
    virtual void log( int level, const QString& message ) = 0;
};

/// Log to @p host, as an error, that work for the host failed: @p what,
/// because of @p reason.
void logFailure( HostAdapter& host, const char* what, const char* reason ) noexcept;

/// Run @p work for the host, logging to @p host instead of throwing: the
/// host is C, and an exception escaping into it (from an entry point, a host
/// callback or a slot its event loop calls) would terminate LogSquirl.
/// Returns false if it threw.
template <typename Work>
bool guarded( HostAdapter& host, const char* what, Work&& work ) noexcept
{
    try {
        work();
        return true;
    } catch ( const std::exception& e ) {
        logFailure( host, what, e.what() );
    } catch ( ... ) {
        logFailure( host, what, "unknown exception" );
    }
    return false;
}

} // namespace nats_monitor

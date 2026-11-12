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
 * @file plugin.h
 * @brief Global plugin state shared across all translation units.
 *
 * The LogSquirl plugin ABI is pure C: the host calls free functions
 * (logsquirl_plugin_init, logsquirl_plugin_shutdown, …) with no object
 * context, so a small set of globals bridges the C entry points and the
 * C++/Qt implementation behind them.
 */

#pragma once

#include "logsquirl_plugin_api.h"

#include <QPointer>
#include <QString>

#include <exception>

namespace nats_monitor {

class ConnectionDialog;
class ProfileStore;
class Sessions;
class SidebarWidget;

/**
 * Global plugin state.
 *
 * Populated by logsquirl_plugin_init(), cleared by logsquirl_plugin_shutdown().
 * Only accessed from the main (GUI) thread.
 */
struct PluginState {
    const LogSquirlHostApi* api = nullptr; ///< Host API function table.
    void* handle = nullptr;                ///< Opaque plugin instance handle.
    Sessions* sessions = nullptr;          ///< The captures, created by init.
    ProfileStore* profiles = nullptr;      ///< The saved connections (nats.ini), created by init.
    ConnectionDialog* dialog = nullptr;    ///< The connection dialog, created by init.
    /// The sidebar tab, created by init.  Weak: the host parents it, and a
    /// host window that goes first takes it along.
    QPointer<SidebarWidget> sidebar;
    bool sidebarRegistered = false; ///< The host took sidebar as a tab.
    bool quitting = false; ///< LogSquirl is quitting (aboutToQuit), not just unloading the plugin.
    QString tempRoot;      ///< Where temporary directories go; empty: the system's (for tests).
};

/// The single global instance — defined in plugin.cpp.
extern PluginState g_state;

/// Log a message through the host API (no-op if not initialised).
/// The host decodes the message as UTF-8.
void hostLog( int level, const QString& message );

/// The plugin's settings file, nats.ini in its configuration directory;
/// empty without a host to say where that is.  Read by init, which opens
/// the profile store (profiles.h) on it: only the store reads and writes it.
QString settingsFile();

/// The directory the plugin's temporary directories go in (tempdirs.h):
/// PluginState::tempRoot, or the system's.  Read by init, which hands it to
/// Sessions, and by shutdown, which removes them.
QString tempRoot();

/// Show a host notification (no-op if not initialised).
/// The host decodes the message as UTF-8.
void hostNotify( const QString& message );

/// Log that work for the host failed: @p what, because of @p reason.
void logFailure( const char* what, const char* reason ) noexcept;

/// Run @p work for the host, logging instead of throwing: the host is C, and
/// an exception escaping into it (from an entry point, a host callback or a
/// slot its event loop calls) would terminate LogSquirl.  Returns false if
/// it threw.
template <typename Work>
bool guarded( const char* what, Work&& work ) noexcept
{
    try {
        work();
        return true;
    } catch ( const std::exception& e ) {
        logFailure( what, e.what() );
    } catch ( ... ) {
        logFailure( what, "unknown exception" );
    }
    return false;
}

} // namespace nats_monitor

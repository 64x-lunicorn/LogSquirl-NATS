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

#include <QString>

namespace nats_monitor {

class ConnectionDialog;
class StatusWidget;

/**
 * Global plugin state.
 *
 * Populated by logsquirl_plugin_init(), cleared by logsquirl_plugin_shutdown().
 * Only accessed from the main (GUI) thread.
 */
struct PluginState {
    const LogSquirlHostApi* api = nullptr; ///< Host API function table.
    void* handle = nullptr;                ///< Opaque plugin instance handle.
    ConnectionDialog* dialog = nullptr;    ///< The NATS Monitor dialog, created by init.
    StatusWidget* statusWidget = nullptr;  ///< In the host's status bar, created by init.
    bool statusWidgetRegistered = false;   ///< The host took statusWidget.
    bool quitting = false; ///< LogSquirl is quitting (aboutToQuit), not just unloading the plugin.
    QString tempRoot;      ///< Where temporary directories go; empty: the system's (for tests).
};

/// The single global instance — defined in plugin.cpp.
extern PluginState g_state;

/// Log a message through the host API (no-op if not initialised).
/// The host decodes the message as UTF-8.
void hostLog( int level, const QString& message );

/// Show a host notification (no-op if not initialised).
/// The host decodes the message as UTF-8.
void hostNotify( const QString& message );

} // namespace nats_monitor

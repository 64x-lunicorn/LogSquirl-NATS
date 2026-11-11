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
 * @file plugin.cpp
 * @brief C ABI entry points for the LogSquirl NATS Monitor plugin.
 *
 * The four exported symbols of a LogSquirl plugin:
 *
 *   - logsquirl_plugin_get_info()   → static metadata
 *   - logsquirl_plugin_init()       → store host API, register menu action
 *   - logsquirl_plugin_shutdown()   → release resources, clear state
 *   - logsquirl_plugin_configure()  → plugin settings (none yet)
 *
 * No exception may leave an entry point or a host callback: the host is C,
 * and an exception escaping into it terminates LogSquirl.  Their work runs
 * through guarded(), which logs the failure instead.
 *
 * @see logsquirl_plugin_api.h for the full host API reference.
 */

#include "plugin.h"

#include <nats.h>
#include <openssl/crypto.h>

#include <exception>

// ── Global state ─────────────────────────────────────────────────────────

namespace nats_monitor {
PluginState g_state;

void hostLog( int level, const QString& message )
{
    if ( g_state.api && g_state.handle ) {
        g_state.api->log_message( g_state.handle, level, message.toUtf8().constData() );
    }
}
} // namespace nats_monitor

// ── Plugin metadata ──────────────────────────────────────────────────────

/// Static plugin info.  All strings have static storage duration.
static const LogSquirlPluginInfo kPluginInfo = {
    /* id          */ "io.github.logsquirl.nats",
    /* name        */ "NATS Monitor",
    /* version     */ LOGSQUIRL_PLUGIN_VERSION,
    /* description */ "Stream NATS messages into a live, searchable LogSquirl tab",
    /* author      */ "LogSquirl Contributors",
    /* license     */ "GPL-3.0-or-later",
    /* type        */ LOGSQUIRL_PLUGIN_UI,
    /* api_version */ LOGSQUIRL_PLUGIN_API_VERSION,
};

// ── Internal helpers ─────────────────────────────────────────────────────

/// Log that work for the host failed.
static void logFailure( const char* what, const char* reason ) noexcept
{
    try {
        nats_monitor::hostLog( LOGSQUIRL_LOG_ERROR,
                               QStringLiteral( "NATS Monitor plugin: %1 failed: %2" )
                                   .arg( what, QString::fromUtf8( reason ) ) );
    } catch ( ... ) {
        // Nothing left to report it with.
    }
}

/// Run work for the host, logging instead of throwing: the host is C, and
/// an exception escaping into it would terminate LogSquirl.
/// Returns false if it threw.
template <typename Work>
static bool guarded( const char* what, Work&& work ) noexcept
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

// ── Menu action callback ─────────────────────────────────────────────────

/// Called when the user clicks "NATS Monitor…" in the Plugins menu.  The
/// connection dialog comes with the next ticket; until then it does nothing.
static void showNatsMonitor( void* /* userData */ )
{
    guarded( "showing the NATS Monitor dialog", [] {} );
}

// ── Exported C entry points ──────────────────────────────────────────────

extern "C" {

LOGSQUIRL_PLUGIN_EXPORT void logsquirl_plugin_shutdown( void );

/**
 * Return static plugin metadata.  The pointer is valid for the lifetime of
 * the shared library.
 */
LOGSQUIRL_PLUGIN_EXPORT const LogSquirlPluginInfo* logsquirl_plugin_get_info( void )
{
    return &kPluginInfo;
}

/**
 * Initialise the plugin.
 *
 * @param api     Host API function table — valid until shutdown().
 * @param handle  Opaque handle — pass back to every host API call.
 * @return 0 on success, non-zero on failure.
 *
 * Registers the "NATS Monitor…" menu action.  A failure is logged and
 * undone (shutdown()), and init returns 1.
 */
LOGSQUIRL_PLUGIN_EXPORT int logsquirl_plugin_init( const LogSquirlHostApi* api, void* handle )
{
    if ( !api || !handle ) {
        return 1;
    }

    nats_monitor::g_state.api = api;
    nats_monitor::g_state.handle = handle;

    const bool ok = guarded( "initialisation", [ api, handle ] {
        api->log_message( handle, LOGSQUIRL_LOG_INFO, "NATS Monitor plugin initialising…" );

        api->register_menu_action( handle, "Plugins", "NATS Monitor…", &showNatsMonitor, nullptr );

        // Which nats.c and OpenSSL the plugin carries: both are linked in.
        nats_monitor::hostLog(
            LOGSQUIRL_LOG_INFO,
            QStringLiteral( "NATS Monitor plugin ready (nats.c %1, %2)." )
                .arg( QString::fromUtf8( nats_GetVersion() ),
                      QString::fromUtf8( OpenSSL_version( OPENSSL_VERSION ) ) ) );
    } );

    if ( !ok ) {
        logsquirl_plugin_shutdown();
        return 1;
    }
    return 0;
}

/**
 * Shut down the plugin and release its resources.  After this returns, no
 * host API calls may be made.
 */
LOGSQUIRL_PLUGIN_EXPORT void logsquirl_plugin_shutdown( void )
{
    guarded( "shutdown", [] {
        nats_monitor::hostLog( LOGSQUIRL_LOG_INFO, "NATS Monitor plugin shutting down…" );
    } );

    nats_monitor::g_state.api = nullptr;
    nats_monitor::g_state.handle = nullptr;
}

/**
 * Open the plugin's settings.  It has none yet.
 *
 * @param parent_widget  Cast of a QWidget* the plugin can use as dialog parent.
 */
LOGSQUIRL_PLUGIN_EXPORT void logsquirl_plugin_configure( void* /* parent_widget */ )
{
    guarded( "configuring", [] {
        nats_monitor::hostLog( LOGSQUIRL_LOG_INFO, "NATS Monitor has no settings yet." );
    } );
}

} // extern "C"

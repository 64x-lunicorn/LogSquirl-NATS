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
 *   - logsquirl_plugin_init()       → store host API, create the sessions, the profile store,
 *                                     the dialog and the sidebar tab, register the menu action
 *                                     and the tab
 *   - logsquirl_plugin_shutdown()   → stop the sessions, close nats.c, clear state
 *   - logsquirl_plugin_configure()  → plugin settings (none yet)
 *
 * No exception may leave an entry point or a host callback: the host is C,
 * and an exception escaping into it terminates LogSquirl.  Their work runs
 * through guarded(), which logs the failure instead.
 *
 * @see logsquirl_plugin_api.h for the full host API reference.
 */

// First: on Windows nats.h includes winsock2.h, which must come before any
// windows.h.
#include <nats.h>

#include "plugin.h"

#include "connectiondialog.h"
#include "profiles.h"
#include "sessions.h"
#include "sidebarwidget.h"
#include "tempdirs.h"

#include <openssl/crypto.h>

#include <QApplication>
#include <QDir>
#include <QWindow>

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

QString settingsFile()
{
    if ( !g_state.api || !g_state.handle || !g_state.api->get_config_dir ) {
        return {};
    }
    const char* dir = g_state.api->get_config_dir( g_state.handle );
    if ( !dir || !*dir ) {
        return {};
    }
    return QString::fromUtf8( dir ) + QStringLiteral( "/nats.ini" );
}

QString tempRoot()
{
    return g_state.tempRoot.isEmpty() ? QDir::tempPath() : g_state.tempRoot;
}

void logFailure( const char* what, const char* reason ) noexcept
{
    logFailure( pluginHost(), what, reason );
}

void hostNotify( const QString& message )
{
    if ( g_state.api && g_state.handle ) {
        g_state.api->show_notification( g_state.handle, message.toUtf8().constData() );
    }
}

namespace {

/// The host adapter on the host API of PluginState.
class PluginHost : public HostAdapter {
public:
    void openFollowed( const QString& path ) override
    {
        if ( g_state.api && g_state.handle ) {
            g_state.api->open_file( g_state.handle, path.toUtf8().constData(), 1 );
        }
    }

    void notify( const QString& message ) override
    {
        hostNotify( message );
    }

    void log( int level, const QString& message ) override
    {
        hostLog( level, message );
    }
};

} // namespace

HostAdapter& pluginHost()
{
    static PluginHost host; // stateless: it reads PluginState at each call
    return host;
}

/// How long shutdown waits for nats.c's threads to end before the host may
/// unload the library.
constexpr int64_t kLibraryCloseTimeoutMs = 5000;
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

using nats_monitor::guarded;

// ── Menu action callback ─────────────────────────────────────────────────

/// Show the connection dialog, on top of the window it was opened from.
static void showDialog()
{
    auto* dialog = nats_monitor::g_state.dialog;
    if ( !dialog ) {
        return;
    }

    // The dialog is created parentless in init() and deleted in shutdown(),
    // so it must not become a child of a main window that may be destroyed
    // first.  A transient parent keeps it on top without handing over
    // ownership.
    auto* window = QApplication::activeWindow();
    if ( window && window != dialog ) {
        dialog->winId(); // creates the native window, and so windowHandle()
        dialog->windowHandle()->setTransientParent( window->windowHandle() );
    }

    dialog->show();
    dialog->raise();
    dialog->activateWindow();
}

/// Called when the user clicks "NATS Monitor…" in the Plugins menu: the
/// dialog as it was left, as LogSquirl-Serial's menu entry next to its tab.
static void showNatsMonitor( void* /* userData */ )
{
    guarded( "showing the NATS Monitor dialog", [] { showDialog(); } );
}

/// Create the sidebar tab and connect it to the dialog, which opens for
/// what is asked in the tab: new and edit.  Both follow the profile store.
static void createSidebar()
{
    auto& st = nats_monitor::g_state;
    st.sidebar = new nats_monitor::SidebarWidget( st.profiles, st.sessions );
    auto* sidebar = st.sidebar.data();
    auto* dialog = st.dialog;
    QObject::connect( sidebar, &nats_monitor::SidebarWidget::newConnectionRequested, dialog,
                      [ dialog ] {
                          guarded( "opening a new connection", [ dialog ] {
                              dialog->newConnection();
                              showDialog();
                          } );
                      } );
    QObject::connect( sidebar, &nats_monitor::SidebarWidget::editRequested, dialog,
                      [ dialog ]( const QString& name ) {
                          guarded( "editing a connection", [ dialog, &name ] {
                              dialog->editConnection( name );
                              showDialog();
                          } );
                      } );
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
 * Registers the sidebar tab "NATS" and the "NATS Monitor…" menu action.
 * A failure is logged and undone (shutdown()), and init returns 1.
 */
LOGSQUIRL_PLUGIN_EXPORT int logsquirl_plugin_init( const LogSquirlHostApi* api, void* handle )
{
    if ( !api || !handle ) {
        return 1;
    }

    nats_monitor::g_state.api = api;
    nats_monitor::g_state.handle = handle;
    nats_monitor::g_state.quitting = false;

    const bool ok = guarded( "initialisation", [ api, handle ] {
        api->log_message( handle, LOGSQUIRL_LOG_INFO, "NATS Monitor plugin initialising…" );

        // Files of LogSquirl processes that ended without removing them,
        // e.g. after a crash: no tab can show them any more.
        nats_monitor::removeStaleTempDirs( nats_monitor::tempRoot() );

        auto& st = nats_monitor::g_state;
        st.sessions
            = new nats_monitor::Sessions( nats_monitor::pluginHost(), nats_monitor::tempRoot() );
        // The one owner of nats.ini; it deletes no connection that runs.
        st.profiles = new nats_monitor::ProfileStore(
            nats_monitor::settingsFile(), [ sessions = st.sessions ]( const QString& name ) {
                return sessions->isRunning( name );
            } );
        st.dialog = new nats_monitor::ConnectionDialog( st.profiles, st.sessions );

        // The host shuts the plugin down both when LogSquirl quits (after
        // aboutToQuit) and when the plugin is disabled or updated at
        // runtime, with the tabs left open; only in the first case may the
        // temporary files go.
        if ( auto* app = QCoreApplication::instance() ) {
            QObject::connect( app, &QCoreApplication::aboutToQuit, st.sessions,
                              [] { nats_monitor::g_state.quitting = true; } );
        }

        api->register_menu_action( handle, "Plugins", "NATS Monitor…", &showNatsMonitor, nullptr );

        // The saved connections as tiles, started and stopped there.
        createSidebar();
        api->register_sidebar_tab( handle, "NATS", static_cast<void*>( st.sidebar.data() ) );
        st.sidebarRegistered = true;

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
    auto& st = nats_monitor::g_state;
    // Each step on its own, so that one failing does not skip the others.
    guarded( "shutdown", [] {
        nats_monitor::hostLog( LOGSQUIRL_LOG_INFO, "NATS Monitor plugin shutting down…" );
    } );

    // The host must let go of the tab before it is deleted; it goes before
    // the sessions it shows.  Gone already (a host window deleted it),
    // there is nothing to unregister or delete.
    if ( st.sidebarRegistered && st.sidebar ) {
        guarded( "unregistering the sidebar tab", [ &st ] {
            if ( st.api && st.handle ) {
                st.api->unregister_sidebar_tab( st.handle,
                                                static_cast<void*>( st.sidebar.data() ) );
            }
        } );
    }
    st.sidebarRegistered = false;
    if ( st.sidebar ) {
        guarded( "shutdown", [ &st ] { delete st.sidebar.data(); } );
    }
    st.sidebar = nullptr;

    if ( st.dialog ) {
        guarded( "shutdown", [ &st ] { delete st.dialog; } );
        st.dialog = nullptr;
    }
    // After the tab and the dialog, which use it; before the sessions it asks.
    if ( st.profiles ) {
        guarded( "shutdown", [ &st ] { delete st.profiles; } );
        st.profiles = nullptr;
    }
    if ( st.sessions ) {
        // Stops the sessions: their files are flushed and kept for the tabs.
        guarded( "stopping the NATS sessions", [ &st ] { st.sessions->stopAll(); } );
        guarded( "shutdown", [ &st ] { delete st.sessions; } );
        st.sessions = nullptr;
    }

    // The tabs close with LogSquirl: remove the files of every instance of
    // the plugin in this process, also those of instances before a runtime
    // disable or update, which only the directory names remember.
    if ( st.quitting ) {
        guarded( "removing temporary files",
                 [] { nats_monitor::removeOwnTempDirs( nats_monitor::tempRoot() ); } );
    }

    // nats.c's threads run code of this library: they must have ended
    // before the host unloads it.  Every connection is destroyed by now.
    guarded( "closing nats.c", [] {
        if ( nats_CloseAndWait( nats_monitor::kLibraryCloseTimeoutMs ) == NATS_TIMEOUT ) {
            nats_monitor::hostLog( LOGSQUIRL_LOG_WARNING,
                                   "NATS Monitor plugin: nats.c did not close in time." );
        }
    } );

    st.api = nullptr;
    st.handle = nullptr;
}

/**
 * Open the plugin's settings.  It has none yet: the connections are set up
 * in the sidebar tab.
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

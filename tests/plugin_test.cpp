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
 * @file plugin_test.cpp
 * @brief BDD tests for the plugin's lifecycle through its C entry points.
 *
 * Drives logsquirl_plugin_init() / logsquirl_plugin_shutdown() against a
 * FakeHost, and clicks the menu entry the plugin registers.
 */

// First: on Windows nats.h includes winsock2.h, which must come before any
// windows.h.
#include <nats.h>

#include <catch2/catch.hpp>

#include "fakehost.h"
#include "natsserver.h"
#include "sessiontest.h"

#include <QApplication>
#include <QDialog>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>

using nats_test::FakeHost;
using nats_test::NatsServer;
using nats_test::Publisher;
using nats_test::readLines;
using nats_test::waitFor;

extern "C" int logsquirl_plugin_init( const LogSquirlHostApi* api, void* handle );
extern "C" void logsquirl_plugin_shutdown( void );
extern "C" void logsquirl_plugin_configure( void* parent_widget );

namespace {

/// The plugin's dialog, if it is open: a top-level QDialog with a subject field.
QDialog* openDialog()
{
    for ( auto* widget : QApplication::topLevelWidgets() ) {
        auto* dialog = qobject_cast<QDialog*>( widget );
        if ( dialog && dialog->isVisible() && dialog->findChild<QLineEdit*>( "subject" ) ) {
            return dialog;
        }
    }
    return nullptr;
}

QLineEdit* field( QDialog* dialog, const char* name )
{
    auto* edit = dialog ? dialog->findChild<QLineEdit*>( name ) : nullptr;
    REQUIRE( edit );
    return edit;
}

QSpinBox* spinBox( QDialog* dialog, const char* name )
{
    auto* spin = dialog ? dialog->findChild<QSpinBox*>( name ) : nullptr;
    REQUIRE( spin );
    return spin;
}

QPushButton* button( QDialog* dialog, const char* name )
{
    auto* pushButton = dialog ? dialog->findChild<QPushButton*>( name ) : nullptr;
    REQUIRE( pushButton );
    return pushButton;
}

/// Open the dialog from the menu, enter @p url and @p subject and click Start.
void startFromDialog( FakeHost& host, const QString& url, const QString& subject )
{
    host.menuActions.first().trigger();
    auto* dialog = openDialog();
    field( dialog, "url" )->setText( url );
    field( dialog, "subject" )->setText( subject );
    button( dialog, "start" )->click();
}

/// Whether the last line of @p path is the stopped marker.
bool endsStopped( const QString& path )
{
    const auto lines = readLines( path );
    return !lines.isEmpty() && lines.last().endsWith( " $NATS-MONITOR - 7 stopped" );
}

/// Wait until the file at @p path is subscribed (its "connected" marker).
bool waitSubscribed( const QString& path )
{
    return waitFor(
        [ & ] {
            const auto lines = readLines( path );
            return !lines.isEmpty() && lines.first().contains( " connected to " );
        },
        10000 );
}

} // namespace

SCENARIO( "the plugin adds NATS Monitor to the Plugins menu", "[plugin]" )
{
    GIVEN( "a host" )
    {
        FakeHost host;

        WHEN( "the plugin is initialised" )
        {
            const int result = logsquirl_plugin_init( host.api(), host.handle() );

            THEN( "it succeeds and registers one menu entry, NATS Monitor…" )
            {
                REQUIRE( result == 0 );
                REQUIRE( host.menuActions.size() == 1 );
                REQUIRE( host.menuActions.first().menuPath == "Plugins" );
                REQUIRE( host.menuActions.first().label == QString::fromUtf8( "NATS Monitor…" ) );
            }

            AND_WHEN( "the user clicks the entry" )
            {
                REQUIRE( host.menuActions.size() == 1 );
                host.menuActions.first().trigger();

                THEN( "the dialog opens with the default server and no subject, and nothing "
                      "is captured yet" )
                {
                    auto* dialog = openDialog();
                    REQUIRE( dialog );
                    REQUIRE( dialog->isVisible() );
                    REQUIRE( field( dialog, "url" )->text() == "nats://localhost:4222" );
                    REQUIRE( field( dialog, "subject" )->text().isEmpty() );
                    REQUIRE( spinBox( dialog, "maxBinaryBytes" )->value() == 256 );
                    REQUIRE( host.openedFiles.isEmpty() );
                    REQUIRE_FALSE( host.logs.join( '\n' ).contains( "failed" ) );
                }

                AND_WHEN( "Start is clicked without a subject" )
                {
                    button( openDialog(), "start" )->click();

                    THEN( "nothing is opened, and the dialog says why" )
                    {
                        REQUIRE( host.openedFiles.isEmpty() );
                        REQUIRE( openDialog()
                                     ->findChild<QLabel*>( "status" )
                                     ->text()
                                     .contains( "subject" ) );
                    }
                }
            }

            logsquirl_plugin_shutdown();
        }
    }
}

SCENARIO( "a failure during initialisation never reaches the host", "[plugin]" )
{
    GIVEN( "a host whose register_menu_action throws" )
    {
        FakeHost host;
        host.failMenuAction = true;

        WHEN( "the plugin is initialised" )
        {
            int result = 0;
            REQUIRE_NOTHROW( result = logsquirl_plugin_init( host.api(), host.handle() ) );

            THEN( "init reports the failure and logs why" )
            {
                REQUIRE( result != 0 );
                REQUIRE( host.logs.join( '\n' ).contains( "the menu is full" ) );
            }
        }
    }

    GIVEN( "no host API table" )
    {
        THEN( "init refuses" )
        {
            REQUIRE( logsquirl_plugin_init( nullptr, nullptr ) != 0 );
        }
    }
}

SCENARIO( "configure and shutdown are safe without a running session", "[plugin]" )
{
    GIVEN( "an initialised plugin" )
    {
        FakeHost host;
        REQUIRE( logsquirl_plugin_init( host.api(), host.handle() ) == 0 );

        THEN( "configure and shutdown do not throw" )
        {
            REQUIRE_NOTHROW( logsquirl_plugin_configure( nullptr ) );
            REQUIRE_NOTHROW( logsquirl_plugin_shutdown() );
        }
    }
}

SCENARIO( "capturing from the dialog: Start, Stop, Start", "[plugin][server]" )
{
    SKIP_WITHOUT_NATS_SERVER();

    GIVEN( "an initialised plugin and a server" )
    {
        NatsServer server;
        FakeHost host;
        REQUIRE( logsquirl_plugin_init( host.api(), host.handle() ) == 0 );

        WHEN( "the user starts a capture from the dialog" )
        {
            startFromDialog( host, server.url(), "orders.>" );

            THEN( "the session's file opens in a followed tab, and a published message is "
                  "a line of it" )
            {
                REQUIRE( host.openedFiles.size() == 1 );
                REQUIRE( host.openedFollow.first() );
                REQUIRE( host.notifications.join( '\n' ).contains( "orders.> started" ) );
                const auto path = host.openedFiles.first();
                REQUIRE( waitSubscribed( path ) );
                {
                    Publisher publisher( server.url() );
                    publisher.publish( "orders.eu.created", "first" );
                }
                REQUIRE(
                    waitFor( [ & ] { return readLines( path ).last().endsWith( " first" ); } ) );
            }

            AND_WHEN( "the user clicks Stop, then Start again" )
            {
                REQUIRE( host.openedFiles.size() == 1 );
                const auto first = host.openedFiles.first();
                REQUIRE( waitSubscribed( first ) );
                button( openDialog(), "stop" )->click();

                THEN( "the first file is kept and ends with the stopped marker, and the second "
                      "capture opens a new tab" )
                {
                    REQUIRE( waitFor( [ & ] { return endsStopped( first ); } ) );
                    REQUIRE( host.notifications.last()
                             == QStringLiteral( "NATS capture of orders.> stopped." ) );
                    button( openDialog(), "start" )->click();
                    REQUIRE( host.openedFiles.size() == 2 );
                    const auto second = host.openedFiles.last();
                    REQUIRE( second != first );
                    REQUIRE( waitSubscribed( second ) );
                    {
                        Publisher publisher( server.url() );
                        publisher.publish( "orders.us.created", "second" );
                    }
                    REQUIRE( waitFor(
                        [ & ] { return readLines( second ).last().endsWith( " second" ); } ) );
                    REQUIRE( QFileInfo::exists( first ) );
                    REQUIRE_FALSE( readLines( first ).join( '\n' ).contains( "second" ) );
                }
            }
        }

        logsquirl_plugin_shutdown();
    }
}

SCENARIO( "the cut length of binary payloads is set in the dialog", "[plugin][server]" )
{
    SKIP_WITHOUT_NATS_SERVER();

    GIVEN( "an initialised plugin and a server" )
    {
        NatsServer server;
        FakeHost host;
        REQUIRE( logsquirl_plugin_init( host.api(), host.handle() ) == 0 );

        WHEN( "the user sets it to 3 bytes and starts a capture" )
        {
            host.menuActions.first().trigger();
            spinBox( openDialog(), "maxBinaryBytes" )->setValue( 3 );
            startFromDialog( host, server.url(), "bin" );

            THEN( "a binary payload is cut after 3 bytes" )
            {
                REQUIRE( host.openedFiles.size() == 1 );
                const auto path = host.openedFiles.first();
                REQUIRE( waitSubscribed( path ) );
                {
                    Publisher publisher( server.url() );
                    publisher.publish( "bin", QByteArray( "\x00\x01\x02\x03\xff", 5 ) );
                }
                REQUIRE( waitFor( [ & ] {
                    return readLines( path ).last().endsWith( " bin - 5 hex:000102" );
                } ) );
            }
        }

        logsquirl_plugin_shutdown();
    }
}

SCENARIO( "a capture that fails is notified", "[plugin][server]" )
{
    SKIP_WITHOUT_NATS_SERVER();

    GIVEN( "an initialised plugin and a URL where no server listens" )
    {
        int port = 0;
        {
            NatsServer server; // a port that was free a moment ago
            port = server.port();
        }
        const QString url = QStringLiteral( "nats://127.0.0.1:%1" ).arg( port );
        FakeHost host;
        REQUIRE( logsquirl_plugin_init( host.api(), host.handle() ) == 0 );

        WHEN( "the user starts a capture" )
        {
            startFromDialog( host, url, "x" );

            THEN( "the host shows a notification saying why" )
            {
                REQUIRE( waitFor(
                    [ & ] {
                        return !host.notifications.isEmpty()
                               && host.notifications.last().contains( " failed: " );
                    },
                    10000 ) );
                REQUIRE( host.notifications.last().contains( url ) );
                REQUIRE_FALSE( button( openDialog(), "stop" )->isEnabled() );
            }
        }

        logsquirl_plugin_shutdown();
    }
}

SCENARIO( "unloading the plugin or quitting LogSquirl while capturing stops cleanly",
          "[plugin][server]" )
{
    SKIP_WITHOUT_NATS_SERVER();

    GIVEN( "a capture running from the dialog" )
    {
        NatsServer server;
        FakeHost host;
        REQUIRE( logsquirl_plugin_init( host.api(), host.handle() ) == 0 );
        startFromDialog( host, server.url(), ">" );
        REQUIRE( host.openedFiles.size() == 1 );
        const auto path = host.openedFiles.first();
        REQUIRE( waitSubscribed( path ) );
        {
            Publisher publisher( server.url() );
            publisher.publish( "a", "captured" );
        }
        REQUIRE( waitFor( [ & ] { return readLines( path ).last().endsWith( " captured" ); } ) );

        WHEN( "the plugin is disabled or updated, LogSquirl running on" )
        {
            QElapsedTimer timer;
            timer.start();
            logsquirl_plugin_shutdown();

            THEN( "the session has stopped quickly, its file is flushed and kept for the tab" )
            {
                REQUIRE( timer.elapsed() < 5000 );
                REQUIRE( endsStopped( path ) );
                REQUIRE( readLines( path ).join( '\n' ).contains( " a - 8 captured" ) );
            }
        }

        WHEN( "LogSquirl quits, which shuts the plugin down" )
        {
            QMetaObject::invokeMethod( QCoreApplication::instance(), "aboutToQuit" );
            logsquirl_plugin_shutdown();

            THEN( "the temporary directory is removed" )
            {
                REQUIRE_FALSE( QFileInfo::exists( QFileInfo( path ).path() ) );
                REQUIRE( QDir( host.tempRoot() ).isEmpty() );
            }
        }
    }
}

SCENARIO( "initialisation removes the temporary files of LogSquirl processes that ended",
          "[plugin]" )
{
    GIVEN( "a temporary directory of a process that no longer runs" )
    {
        FakeHost host;
        // Above any process ID (2^22 on Linux, 2^32 on Windows; tempdirs.cpp
        // takes larger ones for ended processes), so no process has it.
        const QString stale = host.tempRoot() + "/logsquirl-nats-9999999999-AbC123";
        REQUIRE( QDir().mkpath( stale ) );

        WHEN( "the plugin is initialised" )
        {
            REQUIRE( logsquirl_plugin_init( host.api(), host.handle() ) == 0 );

            THEN( "the directory is removed" )
            {
                REQUIRE_FALSE( QFileInfo::exists( stale ) );
            }

            logsquirl_plugin_shutdown();
        }
    }
}

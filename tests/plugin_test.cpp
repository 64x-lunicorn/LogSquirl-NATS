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

#include "testconfig.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTemporaryDir>

using nats_test::FakeHost;
using nats_test::NatsServer;
using nats_test::NatsServerOptions;
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

QComboBox* comboBox( QDialog* dialog, const char* name )
{
    auto* combo = dialog ? dialog->findChild<QComboBox*>( name ) : nullptr;
    REQUIRE( combo );
    return combo;
}

QCheckBox* checkBox( QDialog* dialog, const char* name )
{
    auto* check = dialog ? dialog->findChild<QCheckBox*>( name ) : nullptr;
    REQUIRE( check );
    return check;
}

QString statusText( QDialog* dialog )
{
    auto* label = dialog ? dialog->findChild<QLabel*>( "status" ) : nullptr;
    REQUIRE( label );
    return label->text();
}

/// Choose the authentication method whose entry reads @p text.
void chooseAuth( QDialog* dialog, const QString& text )
{
    auto* combo = comboBox( dialog, "authMethod" );
    const int index = combo->findText( text );
    REQUIRE( index >= 0 );
    combo->setCurrentIndex( index );
}

/// Choose the saved profile @p name.
void chooseProfile( QDialog* dialog, const QString& name )
{
    auto* combo = comboBox( dialog, "profile" );
    const int index = combo->findText( name );
    REQUIRE( index >= 0 );
    combo->setCurrentIndex( index );
}

/// Whether some file under @p dir contains @p text.
bool anyFileContains( const QString& dir, const QByteArray& text )
{
    QDirIterator it( dir, QDir::Files, QDirIterator::Subdirectories );
    while ( it.hasNext() ) {
        QFile file( it.next() );
        if ( file.open( QIODevice::ReadOnly ) && file.readAll().contains( text ) ) {
            return true;
        }
    }
    return false;
}

/// A nats-server started with the configuration @p text.
struct ConfiguredServer {
    explicit ConfiguredServer( const QString& text )
    {
        REQUIRE( dir.isValid() );
        NatsServerOptions options;
        options.configFile = dir.filePath( "server.conf" );
        QFile file( options.configFile );
        REQUIRE( file.open( QIODevice::WriteOnly | QIODevice::Text ) );
        file.write( text.toUtf8() );
        file.close();
        server = std::make_unique<NatsServer>( options );
    }

    QTemporaryDir dir;
    std::unique_ptr<NatsServer> server;
};

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

SCENARIO( "the pending limits are set in the dialog", "[plugin][server]" )
{
    SKIP_WITHOUT_NATS_SERVER();

    GIVEN( "an initialised plugin and a server" )
    {
        NatsServer server;
        FakeHost host;
        REQUIRE( logsquirl_plugin_init( host.api(), host.handle() ) == 0 );
        host.menuActions.first().trigger();

        THEN( "the dialog proposes 1,000,000 messages and 512 MiB" )
        {
            REQUIRE( spinBox( openDialog(), "pendingMessages" )->value() == 1000000 );
            REQUIRE( spinBox( openDialog(), "pendingMegabytes" )->value() == 512 );
        }

        WHEN( "the user sets a single pending message and starts a capture that is flooded" )
        {
            spinBox( openDialog(), "pendingMessages" )->setValue( 1 );
            startFromDialog( host, server.url(), "flood" );
            REQUIRE( host.openedFiles.size() == 1 );
            const auto path = host.openedFiles.first();
            REQUIRE( waitSubscribed( path ) );
            {
                Publisher publisher( server.url() );
                const QByteArray payload( 200, 'x' );
                for ( int i = 0; i < 50000; ++i ) {
                    publisher.publish( "flood", payload, {}, {}, i == 49999 );
                }
            }
            button( openDialog(), "stop" )->click();

            THEN( "the capture drops as a slow consumer, and says so in the file" )
            {
                REQUIRE( readLines( path ).join( '\n' ).contains( " (slow consumer)" ) );
            }
        }

        logsquirl_plugin_shutdown();
    }
}

SCENARIO( "the size limit is set in the dialog, and each new file opens in a new tab",
          "[plugin][server][rotation]" )
{
    SKIP_WITHOUT_NATS_SERVER();

    GIVEN( "an initialised plugin and a server" )
    {
        NatsServer server;
        FakeHost host;
        REQUIRE( logsquirl_plugin_init( host.api(), host.handle() ) == 0 );
        host.menuActions.first().trigger();

        THEN( "the dialog proposes 256 MiB, and 0 reads as no limit" )
        {
            auto* spin = spinBox( openDialog(), "maxFileMiB" );
            REQUIRE( spin->value() == 256 );
            REQUIRE( spin->suffix() == " MiB" );
            spin->setValue( 0 );
            REQUIRE( spin->text() == "No limit" );
        }

        WHEN( "the user sets 1 MiB and starts a capture that writes more than that" )
        {
            spinBox( openDialog(), "maxFileMiB" )->setValue( 1 );
            startFromDialog( host, server.url(), "big" );
            REQUIRE( host.openedFiles.size() == 1 );
            REQUIRE( waitSubscribed( host.openedFiles.first() ) );
            const auto notificationsBefore = host.notifications.size();
            {
                Publisher publisher( server.url() );
                const QByteArray payload( 1000, 'x' );
                for ( int i = 0; i < 1100; ++i ) {
                    publisher.publish( "big", payload, {}, {}, i % 100 == 99 );
                }
            }

            THEN( "the next file opens in a new followed tab, and the host log says why, "
                  "without a notification" )
            {
                REQUIRE( waitFor( [ & ] { return host.openedFiles.size() == 2; } ) );
                REQUIRE( host.openedFollow.at( 1 ) );
                REQUIRE( QFileInfo( host.openedFiles.at( 1 ) ).fileName() == "nats_big.2.log" );
                REQUIRE( QFileInfo( host.openedFiles.first() ).size() <= 1024 * 1024 );
                REQUIRE( host.logs.last().contains( "size limit of 1 MiB" ) );
                REQUIRE( host.logs.last().contains( host.openedFiles.at( 1 ) ) );
                REQUIRE( host.notifications.size() == notificationsBefore );
            }
        }

        logsquirl_plugin_shutdown();
    }
}

SCENARIO( "the size limit is remembered between sessions", "[plugin][server][rotation]" )
{
    SKIP_WITHOUT_NATS_SERVER();

    GIVEN( "a capture started with a size limit of 7 MiB, then the plugin shut down" )
    {
        NatsServer server;
        FakeHost host;
        REQUIRE( logsquirl_plugin_init( host.api(), host.handle() ) == 0 );
        host.menuActions.first().trigger();
        spinBox( openDialog(), "maxFileMiB" )->setValue( 7 );
        startFromDialog( host, server.url(), "x" );
        button( openDialog(), "stop" )->click();
        logsquirl_plugin_shutdown();

        WHEN( "the plugin is initialised again and its dialog opened" )
        {
            host.menuActions.clear();
            REQUIRE( logsquirl_plugin_init( host.api(), host.handle() ) == 0 );
            host.menuActions.first().trigger();

            THEN( "it proposes 7 MiB" )
            {
                REQUIRE( spinBox( openDialog(), "maxFileMiB" )->value() == 7 );
            }

            logsquirl_plugin_shutdown();
        }
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

SCENARIO( "a wrong credential or certificate is explained in the dialog", "[plugin][server][auth]" )
{
    SKIP_WITHOUT_NATS_SERVER();

    GIVEN( "an initialised plugin and a server that requires a user and password" )
    {
        ConfiguredServer server( "authorization { user: alice, password: s3cret }\n" );
        FakeHost host;
        REQUIRE( logsquirl_plugin_init( host.api(), host.handle() ) == 0 );
        host.menuActions.first().trigger();
        auto* dialog = openDialog();
        chooseAuth( dialog, "User and password" );
        field( dialog, "user" )->setText( "alice" );

        WHEN( "the user starts a capture with a wrong password" )
        {
            field( dialog, "password" )->setText( "wrong" );
            startFromDialog( host, server.server->url(), "x" );

            THEN( "the dialog says the server refused the user name or password" )
            {
                REQUIRE( waitFor(
                    [ & ] {
                        return statusText( dialog ).contains(
                            "the server refused the user name or password" );
                    },
                    10000 ) );
                REQUIRE( button( dialog, "start" )->isEnabled() );
            }
        }

        WHEN( "the user starts a capture with the right password" )
        {
            field( dialog, "password" )->setText( "s3cret" );
            startFromDialog( host, server.server->url(), "x" );

            THEN( "the capture runs" )
            {
                REQUIRE( host.openedFiles.size() == 1 );
                REQUIRE( waitSubscribed( host.openedFiles.first() ) );
            }
        }

        WHEN( "the user starts a capture without a password" )
        {
            startFromDialog( host, server.server->url(), "x" );

            THEN( "nothing is started, and the dialog asks for the password" )
            {
                REQUIRE( host.openedFiles.isEmpty() );
                REQUIRE( statusText( dialog ).contains( "Enter the password" ) );
            }
        }

        logsquirl_plugin_shutdown();
    }

    GIVEN( "an initialised plugin and a server that requires TLS" )
    {
        const QString certs = QStringLiteral( LOGSQUIRL_NATS_TEST_CERTS );
        ConfiguredServer server( QStringLiteral( "tls {\n"
                                                 "  cert_file: \"%1/server-cert.pem\"\n"
                                                 "  key_file: \"%1/server-key.pem\"\n"
                                                 "}\n" )
                                     .arg( certs ) );
        FakeHost host;
        REQUIRE( logsquirl_plugin_init( host.api(), host.handle() ) == 0 );
        host.menuActions.first().trigger();
        auto* dialog = openDialog();

        THEN( "skipping the verification is off and marked as insecure" )
        {
            REQUIRE_FALSE( checkBox( dialog, "skipVerify" )->isChecked() );
            REQUIRE( checkBox( dialog, "skipVerify" )->text().contains( "insecure" ) );
        }

        WHEN( "the user starts a capture over TLS trusting another CA" )
        {
            checkBox( dialog, "tls" )->setChecked( true );
            field( dialog, "caFile" )->setText( certs + "/other-ca-cert.pem" );
            startFromDialog( host, server.server->url(), "x" );

            THEN( "the dialog says the server's certificate is not trusted" )
            {
                REQUIRE( waitFor(
                    [ & ] {
                        return statusText( dialog ).contains(
                            "the server's certificate is not trusted" );
                    },
                    10000 ) );
            }
        }

        WHEN( "the user starts a capture over TLS trusting the test CA" )
        {
            checkBox( dialog, "tls" )->setChecked( true );
            field( dialog, "caFile" )->setText( certs + "/ca-cert.pem" );
            startFromDialog( host, server.server->url(), "x" );

            THEN( "the capture runs" )
            {
                REQUIRE( host.openedFiles.size() == 1 );
                REQUIRE( waitSubscribed( host.openedFiles.first() ) );
            }
        }

        logsquirl_plugin_shutdown();
    }
}

SCENARIO( "connections are saved as profiles, without passwords or tokens", "[plugin]" )
{
    GIVEN( "the dialog filled in with a user, password and TLS, saved as the profile prod" )
    {
        FakeHost host;
        REQUIRE( logsquirl_plugin_init( host.api(), host.handle() ) == 0 );
        host.menuActions.first().trigger();
        auto* dialog = openDialog();
        field( dialog, "url" )->setText( "nats://a:4222, nats://b:4222" );
        chooseAuth( dialog, "User and password" );
        field( dialog, "user" )->setText( "alice" );
        field( dialog, "password" )->setText( "s3cret-password" );
        checkBox( dialog, "tls" )->setChecked( true );
        field( dialog, "caFile" )->setText( "/etc/nats/ca.pem" );
        field( dialog, "certFile" )->setText( "/etc/nats/client.pem" );
        field( dialog, "keyFile" )->setText( "/etc/nats/client-key.pem" );
        field( dialog, "serverName" )->setText( "nats.example.com" );
        comboBox( dialog, "profile" )->setEditText( "prod" );
        button( dialog, "saveProfile" )->click();

        THEN( "the dialog says it is saved, without the password" )
        {
            REQUIRE( statusText( dialog ).contains( "prod" ) );
            REQUIRE( statusText( dialog ).contains( "saved" ) );
        }

        THEN( "no file of the plugin's configuration holds the password" )
        {
            REQUIRE_FALSE( anyFileContains( host.configDir(), "s3cret-password" ) );
            REQUIRE( anyFileContains( host.configDir(), "nats.example.com" ) );
        }

        AND_GIVEN( "a second profile, dev, with a token" )
        {
            field( dialog, "url" )->setText( "nats://localhost:4222" );
            chooseAuth( dialog, "Token" );
            field( dialog, "password" )->setText( "s3cret-token" );
            checkBox( dialog, "tls" )->setChecked( false );
            comboBox( dialog, "profile" )->setEditText( "dev" );
            button( dialog, "saveProfile" )->click();

            THEN( "the token is not saved either" )
            {
                REQUIRE_FALSE( anyFileContains( host.configDir(), "s3cret-token" ) );
            }

            AND_WHEN( "the plugin is initialised again and prod chosen in its dialog" )
            {
                logsquirl_plugin_shutdown();
                host.menuActions.clear();
                REQUIRE( logsquirl_plugin_init( host.api(), host.handle() ) == 0 );
                host.menuActions.first().trigger();
                dialog = openDialog();
                chooseProfile( dialog, "prod" );

                THEN( "its settings are back, the password empty and asked for" )
                {
                    REQUIRE( comboBox( dialog, "profile" )->count() == 2 );
                    REQUIRE( field( dialog, "url" )->text() == "nats://a:4222, nats://b:4222" );
                    REQUIRE( comboBox( dialog, "authMethod" )->currentText()
                             == "User and password" );
                    REQUIRE( field( dialog, "user" )->text() == "alice" );
                    REQUIRE( field( dialog, "password" )->text().isEmpty() );
                    REQUIRE( checkBox( dialog, "tls" )->isChecked() );
                    REQUIRE( field( dialog, "caFile" )->text() == "/etc/nats/ca.pem" );
                    REQUIRE( field( dialog, "certFile" )->text() == "/etc/nats/client.pem" );
                    REQUIRE( field( dialog, "keyFile" )->text() == "/etc/nats/client-key.pem" );
                    REQUIRE( field( dialog, "serverName" )->text() == "nats.example.com" );
                    REQUIRE_FALSE( checkBox( dialog, "skipVerify" )->isChecked() );
                    REQUIRE( statusText( dialog ).contains( "Enter the password" ) );
                }

                AND_WHEN( "dev is chosen" )
                {
                    chooseProfile( dialog, "dev" );

                    THEN( "its settings are back, the token asked for" )
                    {
                        REQUIRE( field( dialog, "url" )->text() == "nats://localhost:4222" );
                        REQUIRE( comboBox( dialog, "authMethod" )->currentText() == "Token" );
                        REQUIRE( field( dialog, "password" )->text().isEmpty() );
                        REQUIRE_FALSE( checkBox( dialog, "tls" )->isChecked() );
                        REQUIRE( statusText( dialog ).contains( "Enter the token" ) );
                    }
                }

                AND_WHEN( "prod is deleted" )
                {
                    button( dialog, "deleteProfile" )->click();

                    THEN( "only dev is left, also after the plugin is initialised again" )
                    {
                        REQUIRE( comboBox( dialog, "profile" )->findText( "prod" ) < 0 );
                        logsquirl_plugin_shutdown();
                        host.menuActions.clear();
                        REQUIRE( logsquirl_plugin_init( host.api(), host.handle() ) == 0 );
                        host.menuActions.first().trigger();
                        auto* again = openDialog();
                        REQUIRE( comboBox( again, "profile" )->count() == 1 );
                        REQUIRE( comboBox( again, "profile" )->itemText( 0 ) == "dev" );
                    }
                }
            }
        }

        logsquirl_plugin_shutdown();
    }

    GIVEN( "the dialog without a profile name" )
    {
        FakeHost host;
        REQUIRE( logsquirl_plugin_init( host.api(), host.handle() ) == 0 );
        host.menuActions.first().trigger();
        auto* dialog = openDialog();

        WHEN( "Save is clicked" )
        {
            button( dialog, "saveProfile" )->click();

            THEN( "nothing is saved, and the dialog asks for a name" )
            {
                REQUIRE( comboBox( dialog, "profile" )->count() == 0 );
                REQUIRE( statusText( dialog ).contains( "name" ) );
            }
        }

        WHEN( "a URL with a password in it is saved as a profile" )
        {
            field( dialog, "url" )->setText( "nats://a:4222, nats://bob:s3cret-url@b:4222" );
            comboBox( dialog, "profile" )->setEditText( "url" );
            button( dialog, "saveProfile" )->click();

            THEN( "nothing is saved, and the dialog says to use the authentication fields" )
            {
                REQUIRE( comboBox( dialog, "profile" )->count() == 0 );
                REQUIRE_FALSE( anyFileContains( host.configDir(), "s3cret-url" ) );
                REQUIRE( statusText( dialog ).contains( "Authentication" ) );
            }
        }

        logsquirl_plugin_shutdown();
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

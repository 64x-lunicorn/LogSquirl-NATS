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
 * FakeHost, and clicks the menu entry, the dialog and the sidebar tab the
 * plugin registers.
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
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QTimer>

#include <functional>
#include <optional>

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
        if ( dialog && dialog->isVisible() && dialog->findChild<QPlainTextEdit*>( "subjects" ) ) {
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

/// The subjects to capture, one a line: "<subject> [<queue group>]".
QPlainTextEdit* subjectsField( QDialog* dialog )
{
    auto* edit = dialog ? dialog->findChild<QPlainTextEdit*>( "subjects" ) : nullptr;
    REQUIRE( edit );
    return edit;
}

/// The dialog's warning about the subjects entered; empty if none.
QString subjectsWarning( QDialog* dialog )
{
    auto* label = dialog ? dialog->findChild<QLabel*>( "subjectsWarning" ) : nullptr;
    REQUIRE( label );
    return label->text();
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

/// The plugin's sidebar tab.
QWidget* sidebar( const FakeHost& host )
{
    REQUIRE( host.sidebarTabs.size() == 1 );
    return host.sidebarTabs.first().widget;
}

/// The sidebar's button @p name.
QPushButton* sidebarButton( const FakeHost& host, const char* name )
{
    auto* pushButton = sidebar( host )->findChild<QPushButton*>( name );
    REQUIRE( pushButton );
    return pushButton;
}

/// The names of the tiles in the sidebar, top to bottom.
QStringList tileNames( const FakeHost& host )
{
    // In the order they were added: children() keeps it.
    QStringList names;
    for ( auto* tile : sidebar( host )->findChildren<QWidget*>( "tile" ) ) {
        names << tile->findChild<QLabel*>( "title" )->text();
    }
    return names;
}

/// The tile of the connection @p name.
QWidget* tile( const FakeHost& host, const QString& name )
{
    for ( auto* widget : sidebar( host )->findChildren<QWidget*>( "tile" ) ) {
        if ( widget->findChild<QLabel*>( "title" )->text() == name ) {
            return widget;
        }
    }
    FAIL( "no tile " << name.toStdString() );
    return nullptr;
}

/// The button @p button of the tile of @p name.
QPushButton* tileButton( const FakeHost& host, const QString& name, const char* button )
{
    auto* pushButton = tile( host, name )->findChild<QPushButton*>( button );
    REQUIRE( pushButton );
    return pushButton;
}

/// What the tile of @p name says about the connection.
QString tileSummary( const FakeHost& host, const QString& name )
{
    return tile( host, name )->findChild<QLabel*>( "summary" )->text();
}

/// What the tile of @p name says its session does or did.
QString tileDetails( const FakeHost& host, const QString& name )
{
    return tile( host, name )->findChild<QLabel*>( "details" )->text();
}

/// Have @p act deal with the next modal dialog once it is open.
void onNextModal( std::function<void( QWidget* )> act )
{
    auto* timer = new QTimer;
    timer->setInterval( 10 );
    QObject::connect( timer, &QTimer::timeout, [ timer, act ] {
        if ( auto* modal = QApplication::activeModalWidget() ) {
            timer->stop();
            timer->deleteLater();
            act( modal );
        }
    } );
    timer->start();
}

/// Answer the next password or token prompt with @p secret, or cancel it.
void answerSecret( const std::optional<QString>& secret )
{
    onNextModal( [ secret ]( QWidget* modal ) {
        auto* input = qobject_cast<QInputDialog*>( modal );
        REQUIRE( input );
        if ( secret ) {
            input->setTextValue( *secret );
            input->accept();
        }
        else {
            input->reject();
        }
    } );
}

/// Answer the next question with @p button.
void answerQuestion( QMessageBox::StandardButton button )
{
    onNextModal( [ button ]( QWidget* modal ) {
        auto* box = qobject_cast<QMessageBox*>( modal );
        REQUIRE( box );
        box->button( button )->click();
    } );
}

/// Save the settings in @p dialog as the connection @p name.
void saveAs( QDialog* dialog, const QString& name )
{
    comboBox( dialog, "profile" )->setEditText( name );
    button( dialog, "saveProfile" )->click();
}

/// Click Start on the tile of @p name, answering a prompt for the password
/// or token with @p secret.
void startTile( FakeHost& host, const QString& name, const std::optional<QString>& secret = {} )
{
    if ( secret ) {
        answerSecret( secret );
    }
    tileButton( host, name, "startStop" )->click();
}

/// Open the dialog from the menu, enter @p url and @p subject, save the
/// settings as the connection @p name and start it from its tile,
/// answering a prompt for the password or token with @p secret.
void startFromDialog( FakeHost& host, const QString& url, const QString& subject,
                      const std::optional<QString>& secret = {},
                      const QString& name = QStringLiteral( "test" ) )
{
    host.menuActions.first().trigger();
    auto* dialog = openDialog();
    field( dialog, "url" )->setText( url );
    subjectsField( dialog )->setPlainText( subject );
    saveAs( dialog, name );
    INFO( "dialog: " << statusText( dialog ).toStdString() );
    REQUIRE( tileNames( host ).contains( name ) );
    startTile( host, name, secret );
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
                    REQUIRE( subjectsField( dialog )->toPlainText().isEmpty() );
                    REQUIRE( spinBox( dialog, "maxBinaryBytes" )->value() == 256 );
                    REQUIRE( host.openedFiles.isEmpty() );
                    REQUIRE_FALSE( host.logs.join( '\n' ).contains( "failed" ) );
                }

                AND_WHEN( "the settings are saved without a subject" )
                {
                    saveAs( openDialog(), "x" );

                    THEN( "nothing is saved, and the dialog says why" )
                    {
                        REQUIRE( tileNames( host ).isEmpty() );
                        REQUIRE( statusText( openDialog() ).contains( "subject" ) );
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

SCENARIO( "capturing from a tile: Start, Stop, Start", "[plugin][server]" )
{
    SKIP_WITHOUT_NATS_SERVER();

    GIVEN( "an initialised plugin and a server" )
    {
        NatsServer server;
        FakeHost host;
        REQUIRE( logsquirl_plugin_init( host.api(), host.handle() ) == 0 );

        WHEN( "the user saves a connection in the dialog and starts it from its tile" )
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
                startTile( host, "test" ); // Stop

                THEN( "the first file is kept and ends with the stopped marker, and the second "
                      "capture opens a new tab" )
                {
                    REQUIRE( waitFor( [ & ] { return endsStopped( first ); } ) );
                    REQUIRE( host.notifications.last()
                             == QStringLiteral( "NATS capture of orders.> stopped." ) );
                    startTile( host, "test" );
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
            startTile( host, "test" ); // Stop

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
        startTile( host, "test" ); // Stop
        logsquirl_plugin_shutdown();

        WHEN( "the plugin is initialised again and its dialog opened" )
        {
            host.menuActions.clear();
            host.sidebarTabs.clear();
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

            THEN( "the host shows a notification saying why, and so does the tile" )
            {
                REQUIRE( waitFor(
                    [ & ] {
                        return !host.notifications.isEmpty()
                               && host.notifications.last().contains( " failed: " );
                    },
                    10000 ) );
                REQUIRE( host.notifications.last().contains( url ) );
                REQUIRE( waitFor(
                    [ & ] { return tileDetails( host, "test" ).startsWith( "failed: " ); } ) );
                REQUIRE( tileDetails( host, "test" ).contains( url ) );
                REQUIRE( tileButton( host, "test", "startStop" )->text().contains( "Start" ) );
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

SCENARIO( "a wrong credential or certificate is explained on the tile", "[plugin][server][auth]" )
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

        WHEN( "the user starts a capture and enters a wrong password" )
        {
            startFromDialog( host, server.server->url(), "x", QStringLiteral( "wrong" ) );

            THEN( "the tile says the server refused the user name or password" )
            {
                REQUIRE( waitFor(
                    [ & ] {
                        return tileDetails( host, "test" ).startsWith( "failed: " )
                               && tileDetails( host, "test" )
                                      .contains( "the server refused the user name or password" );
                    },
                    10000 ) );
                REQUIRE( tileButton( host, "test", "startStop" )->text().contains( "Start" ) );
            }
        }

        WHEN( "the user starts a capture and enters the right password" )
        {
            startFromDialog( host, server.server->url(), "x", QStringLiteral( "s3cret" ) );

            THEN( "the capture runs, and the password is in no file of the configuration" )
            {
                REQUIRE( host.openedFiles.size() == 1 );
                REQUIRE( waitSubscribed( host.openedFiles.first() ) );
                REQUIRE_FALSE( anyFileContains( host.configDir(), "s3cret" ) );
            }
        }

        WHEN( "the user starts a capture and cancels the password prompt" )
        {
            answerSecret( std::nullopt );
            startFromDialog( host, server.server->url(), "x" );

            THEN( "nothing is started" )
            {
                REQUIRE( host.openedFiles.isEmpty() );
                REQUIRE( tileButton( host, "test", "startStop" )->text().contains( "Start" ) );
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

            THEN( "the tile says the server's certificate is not trusted" )
            {
                REQUIRE( waitFor(
                    [ & ] {
                        return tileDetails( host, "test" )
                            .contains( "the server's certificate is not trusted" );
                    },
                    10000 ) );
            }
        }

        WHEN( "the user starts a capture over TLS trusting the test CA" )
        {
            checkBox( dialog, "tls" )->setChecked( true );
            field( dialog, "caFile" )->setText( certs + "/ca-cert.pem" );
            startFromDialog( host, server.server->url(), "x" );

            THEN( "the capture runs, and the tile says to which server" )
            {
                REQUIRE( host.openedFiles.size() == 1 );
                REQUIRE( waitSubscribed( host.openedFiles.first() ) );
                REQUIRE( tileSummary( host, "test" ).contains( "TLS on" ) );
                REQUIRE( waitFor(
                    [ & ] { return tileDetails( host, "test" ).contains( "connected to " ); } ) );
            }
        }

        WHEN( "the user starts a capture over TLS skipping the verification" )
        {
            checkBox( dialog, "tls" )->setChecked( true );
            checkBox( dialog, "skipVerify" )->setChecked( true );
            startFromDialog( host, server.server->url(), "x" );

            THEN( "the capture runs, and the host log says the certificate is not verified" )
            {
                REQUIRE( host.openedFiles.size() == 1 );
                REQUIRE( waitSubscribed( host.openedFiles.first() ) );
                REQUIRE( host.logs.join( '\n' ).contains( "is not verified (insecure)" ) );
            }
        }

        WHEN( "the user starts a capture with a tls:// URL, Use TLS unchecked, trusting the test "
              "CA" )
        {
            const auto url = server.server->url().replace( "nats://", "tls://" );
            field( dialog, "url" )->setText( url );
            REQUIRE_FALSE( checkBox( dialog, "tls" )->isChecked() );

            THEN( "the TLS settings can be entered, and they are used: the capture runs" )
            {
                REQUIRE( field( dialog, "caFile" )->isEnabled() );
                field( dialog, "caFile" )->setText( certs + "/ca-cert.pem" );
                startFromDialog( host, url, "x" );
                REQUIRE( host.openedFiles.size() == 1 );
                REQUIRE( waitSubscribed( host.openedFiles.first() ) );
            }
        }

        logsquirl_plugin_shutdown();
    }
}

namespace {

/// Save the dialog's settings with @p url, start the connection, entering
/// @p secret if asked, and wait until its tile says @p text.
void requireStatusAfterStart( FakeHost& host, const QString& url, const QString& text,
                              const std::optional<QString>& secret = {} )
{
    startFromDialog( host, url, "x", secret );
    REQUIRE( waitFor(
        [ & ] {
            const auto details = tileDetails( host, "test" );
            return details.startsWith( "failed: " ) && details.contains( text );
        },
        10000 ) );
    INFO( "tile: " << tileDetails( host, "test" ).toStdString() );
    REQUIRE( tileDetails( host, "test" ).contains( text ) );
    REQUIRE( tileButton( host, "test", "startStop" )->text().contains( "Start" ) );
}

/// The content of the fixture file @p path, trimmed.
QString fixtureText( const QString& path )
{
    QFile file( path );
    REQUIRE( file.open( QIODevice::ReadOnly | QIODevice::Text ) );
    return QString::fromUtf8( file.readAll() ).trimmed();
}

} // namespace

SCENARIO( "a wrong token, NKey, .creds or client certificate is explained on the tile",
          "[plugin][server][auth]" )
{
    SKIP_WITHOUT_NATS_SERVER();
    const QString auth = QStringLiteral( LOGSQUIRL_NATS_TEST_AUTH );
    const QString certs = QStringLiteral( LOGSQUIRL_NATS_TEST_CERTS );

    GIVEN( "an initialised plugin and a server that requires a token" )
    {
        ConfiguredServer server( "authorization { token: t0ken }\n" );
        FakeHost host;
        REQUIRE( logsquirl_plugin_init( host.api(), host.handle() ) == 0 );
        host.menuActions.first().trigger();
        auto* dialog = openDialog();
        chooseAuth( dialog, "Token" );

        WHEN( "the user starts a capture and enters a wrong token" )
        {
            THEN( "the tile says the server refused the token" )
            {
                requireStatusAfterStart( host, server.server->url(), "the server refused the token",
                                         QStringLiteral( "wrong" ) );
            }
        }

        logsquirl_plugin_shutdown();
    }

    GIVEN( "an initialised plugin and a server that knows the fixture NKey user" )
    {
        ConfiguredServer server( QStringLiteral( "authorization { users: [ { nkey: %1 } ] }\n" )
                                     .arg( fixtureText( auth + "/nkey.pub" ) ) );
        FakeHost host;
        REQUIRE( logsquirl_plugin_init( host.api(), host.handle() ) == 0 );
        host.menuActions.first().trigger();
        auto* dialog = openDialog();
        chooseAuth( dialog, "NKey seed file" );

        WHEN( "the user starts a capture with the seed of another NKey" )
        {
            field( dialog, "authFile" )->setText( auth + "/other.seed" );

            THEN( "the tile says the server refused the NKey in that file" )
            {
                requireStatusAfterStart( host, server.server->url(),
                                         "the server refused the NKey in " + auth + "/other.seed" );
            }
        }

        logsquirl_plugin_shutdown();
    }

    GIVEN( "an initialised plugin and a server in operator mode with the fixture account" )
    {
        ConfiguredServer server( fixtureText( auth + "/operator.conf" ) );
        FakeHost host;
        REQUIRE( logsquirl_plugin_init( host.api(), host.handle() ) == 0 );
        host.menuActions.first().trigger();
        auto* dialog = openDialog();
        chooseAuth( dialog, "Credentials file (.creds)" );

        WHEN( "the user starts a capture with the .creds file of an unknown account" )
        {
            field( dialog, "authFile" )->setText( auth + "/other.creds" );

            THEN( "the tile says the server refused the credentials in that file" )
            {
                requireStatusAfterStart( host, server.server->url(),
                                         "the server refused the credentials in " + auth
                                             + "/other.creds" );
            }
        }

        logsquirl_plugin_shutdown();
    }

    GIVEN( "an initialised plugin and a server that requires a client certificate of the test "
           "CA" )
    {
        ConfiguredServer server( QStringLiteral( "tls {\n"
                                                 "  cert_file: \"%1/server-cert.pem\"\n"
                                                 "  key_file: \"%1/server-key.pem\"\n"
                                                 "  ca_file: \"%1/ca-cert.pem\"\n"
                                                 "  verify: true\n"
                                                 "  timeout: 5\n"
                                                 "}\n" )
                                     .arg( certs ) );
        FakeHost host;
        REQUIRE( logsquirl_plugin_init( host.api(), host.handle() ) == 0 );
        host.menuActions.first().trigger();
        auto* dialog = openDialog();
        checkBox( dialog, "tls" )->setChecked( true );
        field( dialog, "caFile" )->setText( certs + "/ca-cert.pem" );

        WHEN( "the user starts a capture without a client certificate" )
        {
            THEN( "the tile says the server requires (or may require) one" )
            {
                // "may": with TLS 1.3 the server closes without saying why.
                requireStatusAfterStart( host, server.server->url(),
                                         "a client certificate, and none is set" );
            }
        }

        WHEN( "the user starts a capture with a client certificate of another CA" )
        {
            field( dialog, "certFile" )->setText( certs + "/other-client-cert.pem" );
            field( dialog, "keyFile" )->setText( certs + "/other-client-key.pem" );

            THEN( "the tile says the server does not (or may not) accept it" )
            {
                requireStatusAfterStart( host, server.server->url(),
                                         "accept the client certificate " + certs
                                             + "/other-client-cert.pem" );
            }
        }

        logsquirl_plugin_shutdown();
    }
}

SCENARIO( "the dialog warns when the subjects of a session overlap", "[plugin][subjects]" )
{
    GIVEN( "the dialog of an initialised plugin" )
    {
        FakeHost host;
        REQUIRE( logsquirl_plugin_init( host.api(), host.handle() ) == 0 );
        host.menuActions.first().trigger();
        auto* dialog = openDialog();

        THEN( "subjects that do not overlap get no warning" )
        {
            for ( const auto* subjects : { "orders.>", "orders.eu.*\nbilling.>", "orders.>\norders",
                                           "*\na.b", "a.*.c\na.b.d", "  \norders.>\n\n" } ) {
                subjectsField( dialog )->setPlainText( QString::fromUtf8( subjects ) );
                INFO( subjects );
                REQUIRE( subjectsWarning( dialog ).isEmpty() );
            }
        }

        WHEN( "one subject covers another" )
        {
            subjectsField( dialog )->setPlainText( "orders.>\norders.eu.*" );

            THEN( "the warning says a message on both is written once for each, and suggests "
                  "dropping the narrower one" )
            {
                const auto warning = subjectsWarning( dialog );
                REQUIRE( warning.contains( "orders.> and orders.eu.* overlap" ) );
                REQUIRE( warning.contains( "once for each" ) );
                REQUIRE( warning.contains( "drop orders.eu.*" ) );
            }

            AND_WHEN( "the narrower one is removed" )
            {
                subjectsField( dialog )->setPlainText( "orders.>" );

                THEN( "the warning goes" )
                {
                    REQUIRE( subjectsWarning( dialog ).isEmpty() );
                }
            }
        }

        WHEN( "the narrower one comes first, in a queue group, and > covers a subject" )
        {
            subjectsField( dialog )->setPlainText( "a.b workers\n>" );

            THEN( "the suggestion is still to drop the narrower one" )
            {
                REQUIRE( subjectsWarning( dialog ).contains( "> and a.b overlap" ) );
                REQUIRE( subjectsWarning( dialog ).contains( "drop a.b" ) );
            }
        }

        WHEN( "two subjects overlap without either covering the other" )
        {
            subjectsField( dialog )->setPlainText( "orders.*.paid\norders.eu.*" );

            THEN( "the warning says they overlap, without telling to drop either" )
            {
                const auto warning = subjectsWarning( dialog );
                REQUIRE( warning.contains( "orders.*.paid and orders.eu.* overlap" ) );
                REQUIRE_FALSE( warning.contains( "drop" ) );
            }
        }

        WHEN( "a subject is entered twice" )
        {
            subjectsField( dialog )->setPlainText( "orders.>\norders.> audit" );

            THEN( "the warning says so" )
            {
                REQUIRE( subjectsWarning( dialog ).contains( "orders.> twice" ) );
            }
        }

        WHEN( "a line has more than a subject and a queue group, and Save is clicked" )
        {
            subjectsField( dialog )->setPlainText( "orders.>\nbilling.> workers extra" );
            saveAs( dialog, "x" );

            THEN( "nothing is saved, and the dialog says which line is wrong" )
            {
                REQUIRE( tileNames( host ).isEmpty() );
                REQUIRE( statusText( dialog ).contains( "billing.> workers extra" ) );
            }
        }

        WHEN( "a subject is not one NATS takes, and Save is clicked" )
        {
            const auto* subject = GENERATE( "a.>.b", "a..b", ".a", "a.", "a.b*", "a.>x" );
            subjectsField( dialog )->setPlainText( QStringLiteral( "a.>\n%1" ).arg( subject ) );
            saveAs( dialog, "x" );

            THEN( "nothing is saved, the dialog says which subject is wrong, and gives no "
                  "overlap warning for it" )
            {
                INFO( subject );
                REQUIRE( tileNames( host ).isEmpty() );
                REQUIRE( statusText( dialog ).contains(
                    QStringLiteral( "%1 is not a valid subject" ).arg( subject ) ) );
                REQUIRE( subjectsWarning( dialog ).isEmpty() );
            }
        }

        logsquirl_plugin_shutdown();
    }
}

namespace {

/// The number before @p word in @p text, whatever the locale groups its
/// digits with; -1 if there is none.
qint64 numberBefore( const QString& text, const QString& word )
{
    const auto match = QRegularExpression( QStringLiteral( "(\\d[\\d.,' \\x{a0}\\x{202f}]*) " )
                                           + QRegularExpression::escape( word ) )
                           .match( text );
    if ( !match.hasMatch() ) {
        return -1;
    }
    return match.captured( 1 ).remove( QRegularExpression( "\\D" ) ).toLongLong();
}

/// The number after @p word in @p text, as numberBefore().
qint64 numberAfter( const QString& text, const QString& word )
{
    const auto match = QRegularExpression( QRegularExpression::escape( word )
                                           + QStringLiteral( " (\\d[\\d.,'\\x{a0}\\x{202f}]*)" ) )
                           .match( text );
    if ( !match.hasMatch() ) {
        return -1;
    }
    return match.captured( 1 ).remove( QRegularExpression( "\\D" ) ).toLongLong();
}

/// Open a new connection from the sidebar, enter @p url and @p subjects and
/// save it as @p name.
void saveConnection( FakeHost& host, const QString& name, const QString& url,
                     const QString& subjects )
{
    sidebarButton( host, "newConnection" )->click();
    auto* dialog = openDialog();
    REQUIRE( dialog );
    field( dialog, "url" )->setText( url );
    subjectsField( dialog )->setPlainText( subjects );
    saveAs( dialog, name );
    INFO( "dialog: " << statusText( dialog ).toStdString() );
    REQUIRE( tileNames( host ).contains( name ) );
}

} // namespace

SCENARIO( "the plugin has a sidebar tab NATS, from init to shutdown", "[plugin][sidebar]" )
{
    GIVEN( "a host" )
    {
        FakeHost host;

        WHEN( "the plugin is initialised" )
        {
            REQUIRE( logsquirl_plugin_init( host.api(), host.handle() ) == 0 );

            THEN( "it registers one sidebar tab, NATS, and no status bar widget" )
            {
                REQUIRE( host.sidebarTabs.size() == 1 );
                REQUIRE( host.sidebarTabs.first().label == "NATS" );
                REQUIRE( host.statusWidgets.isEmpty() );
            }

            THEN( "without a saved connection, the tab has no tile and says how to add one" )
            {
                REQUIRE( tileNames( host ).isEmpty() );
                auto* empty = sidebar( host )->findChild<QLabel*>( "empty" );
                REQUIRE( empty );
                REQUIRE( empty->text().contains( "New connection" ) );
                REQUIRE_FALSE( sidebarButton( host, "stopAll" )->isEnabled() );
            }

            AND_WHEN( "the host's window, which parents the tab, goes before shutdown" )
            {
                {
                    QWidget window;
                    sidebar( host )->setParent( &window );
                } // deletes its children, the tab too

                THEN( "shutdown neither hands the host the deleted tab nor deletes it again" )
                {
                    logsquirl_plugin_shutdown();
                    REQUIRE( host.unregisteredSidebarTabs.isEmpty() );
                }
            }

            AND_WHEN( "the plugin is shut down" )
            {
                auto* widget = sidebar( host );
                logsquirl_plugin_shutdown();

                THEN( "it has unregistered the tab" )
                {
                    REQUIRE( host.sidebarTabs.isEmpty() );
                    REQUIRE( host.unregisteredSidebarTabs == QList<QWidget*>{ widget } );
                }
            }

            logsquirl_plugin_shutdown();
        }
    }
}

SCENARIO( "each saved connection is a tile in the sidebar", "[plugin][sidebar]" )
{
    GIVEN( "an initialised plugin" )
    {
        FakeHost host;
        REQUIRE( logsquirl_plugin_init( host.api(), host.handle() ) == 0 );

        WHEN( "New connection… is clicked" )
        {
            sidebarButton( host, "newConnection" )->click();

            THEN( "the dialog opens, without a name and with the default server" )
            {
                auto* dialog = openDialog();
                REQUIRE( dialog );
                REQUIRE( comboBox( dialog, "profile" )->currentText().isEmpty() );
                REQUIRE( field( dialog, "url" )->text() == "nats://localhost:4222" );
                REQUIRE( subjectsField( dialog )->toPlainText().isEmpty() );
            }
        }

        WHEN( "two connections are saved, prod with a user and TLS, then dev" )
        {
            sidebarButton( host, "newConnection" )->click();
            auto* dialog = openDialog();
            field( dialog, "url" )->setText( "nats://a:4222, nats://b:4222" );
            chooseAuth( dialog, "User and password" );
            field( dialog, "user" )->setText( "alice" );
            checkBox( dialog, "tls" )->setChecked( true );
            subjectsField( dialog )->setPlainText( "orders.>\njobs.* workers" );
            saveAs( dialog, "prod" );
            saveConnection( host, "dev", "nats://localhost:4222", "metrics.>" );

            THEN( "each has a tile, in the order saved, saying what it connects to" )
            {
                REQUIRE( tileNames( host ) == QStringList{ "prod", "dev" } );
                REQUIRE_FALSE( sidebar( host )->findChild<QLabel*>( "empty" )->isVisibleTo(
                    sidebar( host ) ) );
                const auto prod = tileSummary( host, "prod" );
                REQUIRE( prod.contains( "nats://a:4222, nats://b:4222" ) );
                REQUIRE( prod.contains( "User and password" ) );
                REQUIRE( prod.contains( "TLS on" ) );
                REQUIRE( prod.contains( "orders.>, jobs.* (queue workers)" ) );
                const auto dev = tileSummary( host, "dev" );
                REQUIRE( dev.contains( "No credentials" ) );
                REQUIRE( dev.contains( "TLS off" ) );
                REQUIRE( dev.contains( "metrics.>" ) );
                REQUIRE( tileButton( host, "dev", "startStop" )->text().contains( "Start" ) );
                REQUIRE( tileDetails( host, "dev" ).isEmpty() );
            }

            AND_WHEN( "Edit is clicked on prod" )
            {
                openDialog()->hide();
                tileButton( host, "prod", "edit" )->click();

                THEN( "the dialog opens with prod's settings" )
                {
                    auto* again = openDialog();
                    REQUIRE( again );
                    REQUIRE( comboBox( again, "profile" )->currentText() == "prod" );
                    REQUIRE( field( again, "url" )->text() == "nats://a:4222, nats://b:4222" );
                    REQUIRE( field( again, "user" )->text() == "alice" );
                    REQUIRE( subjectsField( again )->toPlainText() == "orders.>\njobs.* workers" );
                }

                AND_WHEN( "its subjects are changed and saved" )
                {
                    subjectsField( openDialog() )->setPlainText( "billing.>" );
                    button( openDialog(), "saveProfile" )->click();

                    THEN( "the same tile shows them" )
                    {
                        REQUIRE( tileNames( host ) == QStringList{ "prod", "dev" } );
                        REQUIRE( tileSummary( host, "prod" ).contains( "billing.>" ) );
                        REQUIRE_FALSE( tileSummary( host, "prod" ).contains( "orders" ) );
                    }
                }
            }

            AND_WHEN( "Delete is clicked on dev, and the question cancelled" )
            {
                answerQuestion( QMessageBox::Cancel );
                tileButton( host, "dev", "delete" )->click();

                THEN( "dev stays" )
                {
                    REQUIRE( tileNames( host ) == QStringList{ "prod", "dev" } );
                }
            }

            AND_WHEN( "Delete is clicked on dev, and confirmed" )
            {
                answerQuestion( QMessageBox::Yes );
                tileButton( host, "dev", "delete" )->click();

                THEN( "its tile goes, and so does the saved connection" )
                {
                    REQUIRE( tileNames( host ) == QStringList{ "prod" } );
                    REQUIRE( comboBox( openDialog(), "profile" )->findText( "dev" ) < 0 );
                    logsquirl_plugin_shutdown();
                    host.menuActions.clear();
                    host.sidebarTabs.clear();
                    REQUIRE( logsquirl_plugin_init( host.api(), host.handle() ) == 0 );
                    REQUIRE( tileNames( host ) == QStringList{ "prod" } );
                }
            }
        }

        logsquirl_plugin_shutdown();
    }
}

SCENARIO( "several connections run side by side, each in its own file and tab",
          "[plugin][server][sidebar]" )
{
    SKIP_WITHOUT_NATS_SERVER();

    GIVEN( "an initialised plugin, a server and two saved connections" )
    {
        NatsServer server;
        FakeHost host;
        REQUIRE( logsquirl_plugin_init( host.api(), host.handle() ) == 0 );
        saveConnection( host, "orders", server.url(), "orders.>\nbilling.* auditors" );
        saveConnection( host, "metrics", server.url(), "metrics.>" );

        WHEN( "both are started from their tiles" )
        {
            startTile( host, "orders" );
            startTile( host, "metrics" );
            REQUIRE( host.openedFiles.size() == 2 );
            const auto first = host.openedFiles.at( 0 );
            const auto second = host.openedFiles.at( 1 );
            REQUIRE( waitSubscribed( first ) );
            REQUIRE( waitSubscribed( second ) );

            THEN( "each tab follows its own file, each captures its subjects only, and each "
                  "tile says it runs, with nothing received or dropped yet" )
            {
                REQUIRE( first != second );
                REQUIRE( host.openedFollow == QList<bool>{ true, true } );
                REQUIRE( readLines( first ).first().contains(
                    "subscribed to orders.>, billing.* (queue auditors)" ) );
                for ( const auto* name : { "orders", "metrics" } ) {
                    REQUIRE( waitFor(
                        [ & ] { return tileDetails( host, name ).startsWith( "running" ); } ) );
                    const auto details = tileDetails( host, name );
                    INFO( details.toStdString() );
                    REQUIRE( numberAfter( details, "received" ) == 0 );
                    REQUIRE( numberAfter( details, "dropped" ) == 0 );
                    REQUIRE( details.contains( "connected to " + server.url() ) );
                    REQUIRE( details.contains(
                        QFileInfo( name == QStringLiteral( "orders" ) ? first : second )
                            .fileName() ) );
                    REQUIRE( tileButton( host, name, "startStop" )->text().contains( "Stop" ) );
                    REQUIRE_FALSE( tileButton( host, name, "delete" )->isEnabled() );
                }
                REQUIRE( sidebarButton( host, "stopAll" )->isEnabled() );
                {
                    Publisher publisher( server.url() );
                    publisher.publish( "orders.eu", "o" );
                    publisher.publish( "billing.eu", "b" );
                    publisher.publish( "metrics.cpu", "m" );
                }
                REQUIRE( waitFor( [ & ] {
                    return readLines( first ).join( '\n' ).contains( " billing.eu - 1 b" )
                           && readLines( second ).join( '\n' ).contains( " metrics.cpu - 1 m" );
                } ) );
                REQUIRE( readLines( first ).join( '\n' ).contains( " orders.eu - 1 o" ) );
                REQUIRE_FALSE( readLines( first ).join( '\n' ).contains( "metrics" ) );
                REQUIRE_FALSE( readLines( second ).join( '\n' ).contains( "orders" ) );
            }

            AND_WHEN( "messages arrive steadily" )
            {
                Publisher publisher( server.url() );
                const bool shown = waitFor(
                    [ & ] {
                        for ( int i = 0; i < 20; ++i ) {
                            publisher.publish( "orders.eu", "x", {}, {}, i == 19 );
                        }
                        return numberBefore( tileDetails( host, "orders" ), "msgs/s" ) > 0;
                    },
                    5000 );

                THEN( "the tile shows the rate and how many it received" )
                {
                    REQUIRE( shown );
                    const auto details = tileDetails( host, "orders" );
                    INFO( details.toStdString() );
                    REQUIRE( numberAfter( details, "received" ) > 0 );
                    REQUIRE( numberAfter( details, "dropped" ) == 0 );
                    REQUIRE( numberBefore( tileDetails( host, "metrics" ), "msgs/s" ) == 0 );
                }
            }

            AND_WHEN( "Stop is clicked on the first" )
            {
                startTile( host, "orders" ); // Stop

                THEN( "it stops, its tile says so, and the other runs on" )
                {
                    REQUIRE( endsStopped( first ) );
                    REQUIRE( host.notifications.last()
                             == "NATS capture of orders.>, billing.* (queue auditors) stopped." );
                    REQUIRE( tileDetails( host, "orders" ).startsWith( "stopped" ) );
                    REQUIRE(
                        tileButton( host, "orders", "startStop" )->text().contains( "Start" ) );
                    REQUIRE( tileButton( host, "orders", "delete" )->isEnabled() );
                    REQUIRE( waitFor( [ & ] {
                        return tileDetails( host, "metrics" ).startsWith( "running" );
                    } ) );
                    {
                        Publisher publisher( server.url() );
                        publisher.publish( "metrics.mem", "still" );
                        publisher.publish( "orders.us", "late" );
                    }
                    REQUIRE( waitFor( [ & ] {
                        return readLines( second ).last().endsWith( " metrics.mem - 5 still" );
                    } ) );
                    REQUIRE_FALSE( readLines( first ).join( '\n' ).contains( "late" ) );
                }
            }

            AND_WHEN( "Stop all is clicked" )
            {
                sidebarButton( host, "stopAll" )->click();

                THEN( "both stop, each file ends with its stopped marker, each tile says so" )
                {
                    REQUIRE( endsStopped( first ) );
                    REQUIRE( endsStopped( second ) );
                    REQUIRE( tileDetails( host, "orders" ).startsWith( "stopped" ) );
                    REQUIRE( tileDetails( host, "metrics" ).startsWith( "stopped" ) );
                    REQUIRE_FALSE( sidebarButton( host, "stopAll" )->isEnabled() );
                }
            }

            AND_WHEN( "a connection that runs is edited and saved" )
            {
                tileButton( host, "metrics", "edit" )->click();
                subjectsField( openDialog() )->setPlainText( "metrics.cpu" );
                button( openDialog(), "saveProfile" )->click();

                THEN( "the tile shows the new subjects, and the dialog says the capture runs on "
                      "with the old ones until started again" )
                {
                    REQUIRE( tileSummary( host, "metrics" ).contains( "metrics.cpu" ) );
                    REQUIRE( statusText( openDialog() ).contains( "runs on" ) );
                    REQUIRE(
                        tileButton( host, "metrics", "startStop" )->text().contains( "Stop" ) );
                }
            }

            AND_WHEN( "the dialog is asked to delete a connection that runs" )
            {
                host.menuActions.first().trigger();
                chooseProfile( openDialog(), "orders" );
                button( openDialog(), "deleteProfile" )->click();

                THEN( "it refuses, and the tile stays" )
                {
                    REQUIRE( statusText( openDialog() ).contains( "stop it" ) );
                    REQUIRE( tileNames( host ) == QStringList{ "orders", "metrics" } );
                }
            }

            AND_WHEN( "LogSquirl quits, which shuts the plugin down" )
            {
                QElapsedTimer timer;
                timer.start();
                QMetaObject::invokeMethod( QCoreApplication::instance(), "aboutToQuit" );
                logsquirl_plugin_shutdown();

                THEN( "every session stops quickly and cleanly, and the files are removed" )
                {
                    REQUIRE( timer.elapsed() < 5000 );
                    REQUIRE_FALSE( QFileInfo::exists( QFileInfo( first ).path() ) );
                    REQUIRE_FALSE( QFileInfo::exists( QFileInfo( second ).path() ) );
                    REQUIRE( QDir( host.tempRoot() ).isEmpty() );
                }
            }

            AND_WHEN( "the plugin is disabled, LogSquirl running on" )
            {
                logsquirl_plugin_shutdown();

                THEN( "every session's file ends with its stopped marker, kept for its tab" )
                {
                    REQUIRE( endsStopped( first ) );
                    REQUIRE( endsStopped( second ) );
                }
            }
        }

        WHEN( "a connection drops as a slow consumer" )
        {
            tileButton( host, "metrics", "edit" )->click();
            spinBox( openDialog(), "pendingMessages" )->setValue( 1 );
            button( openDialog(), "saveProfile" )->click();
            startTile( host, "metrics" );
            REQUIRE( host.openedFiles.size() == 1 );
            REQUIRE( waitSubscribed( host.openedFiles.first() ) );
            {
                Publisher publisher( server.url() );
                const QByteArray payload( 200, 'x' );
                for ( int i = 0; i < 50000; ++i ) {
                    publisher.publish( "metrics.flood", payload, {}, {}, i == 49999 );
                }
            }

            THEN( "its tile shows how many were dropped" )
            {
                REQUIRE( waitFor(
                    [ & ] { return numberAfter( tileDetails( host, "metrics" ), "dropped" ) > 0; },
                    10000 ) );
            }
        }

        logsquirl_plugin_shutdown();
    }
}

SCENARIO( "connections are saved as profiles, without passwords or tokens", "[plugin]" )
{
    GIVEN( "the dialog filled in with a user and TLS, saved as the connection prod" )
    {
        FakeHost host;
        REQUIRE( logsquirl_plugin_init( host.api(), host.handle() ) == 0 );
        host.menuActions.first().trigger();
        auto* dialog = openDialog();
        field( dialog, "url" )->setText( "nats://a:4222, nats://b:4222" );
        chooseAuth( dialog, "User and password" );
        field( dialog, "user" )->setText( "alice" );
        checkBox( dialog, "tls" )->setChecked( true );
        field( dialog, "caFile" )->setText( "/etc/nats/ca.pem" );
        field( dialog, "certFile" )->setText( "/etc/nats/client.pem" );
        field( dialog, "keyFile" )->setText( "/etc/nats/client-key.pem" );
        field( dialog, "serverName" )->setText( "nats.example.com" );
        subjectsField( dialog )->setPlainText( "orders.>" );
        spinBox( dialog, "maxBinaryBytes" )->setValue( 64 );
        spinBox( dialog, "pendingMessages" )->setValue( 5000 );
        spinBox( dialog, "pendingMegabytes" )->setValue( 32 );
        spinBox( dialog, "maxFileMiB" )->setValue( 9 );
        saveAs( dialog, "prod" );

        THEN( "the dialog says it is saved, without the password, and to start it in the "
              "sidebar" )
        {
            REQUIRE( statusText( dialog ).contains( "prod" ) );
            REQUIRE( statusText( dialog ).contains( "saved, without the password" ) );
            REQUIRE( statusText( dialog ).contains( "sidebar" ) );
            REQUIRE( anyFileContains( host.configDir(), "nats.example.com" ) );
        }

        AND_GIVEN( "a second connection, dev, with a token" )
        {
            field( dialog, "url" )->setText( "nats://localhost:4222" );
            chooseAuth( dialog, "Token" );
            checkBox( dialog, "tls" )->setChecked( false );
            saveAs( dialog, "dev" );

            AND_WHEN( "the plugin is initialised again and prod chosen in its dialog" )
            {
                logsquirl_plugin_shutdown();
                host.menuActions.clear();
                host.sidebarTabs.clear();
                REQUIRE( logsquirl_plugin_init( host.api(), host.handle() ) == 0 );
                host.menuActions.first().trigger();
                dialog = openDialog();
                chooseProfile( dialog, "prod" );

                THEN( "its settings are back, and the password is asked for at Start" )
                {
                    REQUIRE( tileNames( host ) == QStringList{ "prod", "dev" } );
                    REQUIRE( comboBox( dialog, "profile" )->count() == 2 );
                    REQUIRE( field( dialog, "url" )->text() == "nats://a:4222, nats://b:4222" );
                    REQUIRE( comboBox( dialog, "authMethod" )->currentText()
                             == "User and password" );
                    REQUIRE( field( dialog, "user" )->text() == "alice" );
                    REQUIRE( checkBox( dialog, "tls" )->isChecked() );
                    REQUIRE( field( dialog, "caFile" )->text() == "/etc/nats/ca.pem" );
                    REQUIRE( field( dialog, "certFile" )->text() == "/etc/nats/client.pem" );
                    REQUIRE( field( dialog, "keyFile" )->text() == "/etc/nats/client-key.pem" );
                    REQUIRE( field( dialog, "serverName" )->text() == "nats.example.com" );
                    REQUIRE_FALSE( checkBox( dialog, "skipVerify" )->isChecked() );
                    REQUIRE( subjectsField( dialog )->toPlainText() == "orders.>" );
                    REQUIRE( spinBox( dialog, "maxBinaryBytes" )->value() == 64 );
                    REQUIRE( spinBox( dialog, "pendingMessages" )->value() == 5000 );
                    REQUIRE( spinBox( dialog, "pendingMegabytes" )->value() == 32 );
                    REQUIRE( spinBox( dialog, "maxFileMiB" )->value() == 9 );
                    REQUIRE( statusText( dialog ).contains( "password is asked for at Start" ) );
                }

                AND_WHEN( "dev is chosen" )
                {
                    chooseProfile( dialog, "dev" );

                    THEN( "its settings are back, the token asked for at Start" )
                    {
                        REQUIRE( field( dialog, "url" )->text() == "nats://localhost:4222" );
                        REQUIRE( comboBox( dialog, "authMethod" )->currentText() == "Token" );
                        REQUIRE_FALSE( checkBox( dialog, "tls" )->isChecked() );
                        REQUIRE( statusText( dialog ).contains( "token is asked for at Start" ) );
                    }
                }

                AND_WHEN( "prod is deleted in the dialog" )
                {
                    button( dialog, "deleteProfile" )->click();

                    THEN( "only dev is left, in the dialog and the sidebar, also after the "
                          "plugin is initialised again" )
                    {
                        REQUIRE( comboBox( dialog, "profile" )->findText( "prod" ) < 0 );
                        REQUIRE( tileNames( host ) == QStringList{ "dev" } );
                        logsquirl_plugin_shutdown();
                        host.menuActions.clear();
                        host.sidebarTabs.clear();
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

    GIVEN( "the dialog without a connection name" )
    {
        FakeHost host;
        REQUIRE( logsquirl_plugin_init( host.api(), host.handle() ) == 0 );
        host.menuActions.first().trigger();
        auto* dialog = openDialog();
        subjectsField( dialog )->setPlainText( "orders.>" );

        WHEN( "Save is clicked" )
        {
            button( dialog, "saveProfile" )->click();

            THEN( "nothing is saved, and the dialog asks for a name" )
            {
                REQUIRE( comboBox( dialog, "profile" )->count() == 0 );
                REQUIRE( tileNames( host ).isEmpty() );
                REQUIRE( statusText( dialog ).contains( "name" ) );
            }
        }

        WHEN( "a URL with a password in it is saved as a connection" )
        {
            field( dialog, "url" )->setText( "nats://a:4222, nats://bob:s3cret-url@b:4222" );
            saveAs( dialog, "url" );

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

SCENARIO( "a connection replays a JetStream stream from its tile", "[plugin][server][replay]" )
{
    SKIP_WITHOUT_NATS_SERVER();

    GIVEN( "an initialised plugin and a server with the streams ORDERS (five messages) and AUDIT" )
    {
        NatsServerOptions options;
        options.jetStream = true;
        NatsServer server( options );
        Publisher publisher( server.url() );
        nats_test::JetStream js( publisher );
        js.addStream( "ORDERS", { "orders.>" } );
        js.addStream( "AUDIT", { "audit.>" } );
        for ( int i = 1; i <= 5; ++i ) {
            js.publish( i % 2 ? "orders.eu" : "orders.us", QByteArray::number( i ) );
        }
        FakeHost host;
        REQUIRE( logsquirl_plugin_init( host.api(), host.handle() ) == 0 );
        sidebarButton( host, "newConnection" )->click();
        auto* dialog = openDialog();
        field( dialog, "url" )->setText( server.url() );

        WHEN( "the user chooses Replay and lists the streams" )
        {
            checkBox( dialog, "replay" )->setChecked( true );
            button( dialog, "listStreams" )->click();

            THEN( "the server's streams are offered" )
            {
                auto* streams = comboBox( dialog, "replayStream" );
                REQUIRE( waitFor( [ & ] { return streams->count() == 2; }, 5000 ) );
                REQUIRE( streams->itemText( 0 ) == "AUDIT" );
                REQUIRE( streams->itemText( 1 ) == "ORDERS" );
                REQUIRE( subjectsField( dialog )->isEnabled() == false );
            }
        }

        WHEN( "the user saves a replay of the last 2 messages on orders.eu of ORDERS" )
        {
            checkBox( dialog, "replay" )->setChecked( true );
            comboBox( dialog, "replayStream" )->setEditText( "ORDERS" );
            field( dialog, "replayFilter" )->setText( "orders.eu" );
            auto* start = comboBox( dialog, "replayStart" );
            start->setCurrentIndex( start->findText( "Last messages" ) );
            spinBox( dialog, "replayLast" )->setValue( 2 );
            saveAs( dialog, "orders" );
            INFO( "dialog: " << statusText( dialog ).toStdString() );

            THEN( "its tile says what it replays" )
            {
                REQUIRE( tileSummary( host, "orders" )
                             .contains( "Replays stream ORDERS, subject orders.eu, the last 2 "
                                        "messages" ) );
            }

            THEN( "Edit shows the replay again" )
            {
                dialog->hide();
                tileButton( host, "orders", "edit" )->click();
                auto* again = openDialog();
                REQUIRE( checkBox( again, "replay" )->isChecked() );
                REQUIRE( comboBox( again, "replayStream" )->currentText() == "ORDERS" );
                REQUIRE( field( again, "replayFilter" )->text() == "orders.eu" );
                REQUIRE( comboBox( again, "replayStart" )->currentText() == "Last messages" );
                REQUIRE( spinBox( again, "replayLast" )->value() == 2 );
            }

            AND_WHEN( "it is started from its tile" )
            {
                startTile( host, "orders" );

                THEN( "messages 3 and 5 are replayed into a followed tab, the tile says the "
                      "replay is complete, and the stream is unchanged" )
                {
                    REQUIRE( host.openedFiles.size() == 1 );
                    REQUIRE( host.openedFollow.first() );
                    const auto path = host.openedFiles.first();
                    REQUIRE( waitFor(
                        [ & ] {
                            return tileDetails( host, "orders" ).contains( "replay complete" );
                        },
                        10000 ) );
                    const auto lines = readLines( path );
                    REQUIRE( lines.size() == 4 );
                    REQUIRE( lines.at( 1 ).endsWith( " orders.eu - #3 1 3" ) );
                    REQUIRE( lines.at( 2 ).endsWith( " orders.eu - #5 1 5" ) );
                    REQUIRE(
                        tileButton( host, "orders", "startStop" )->text().contains( "Start" ) );
                    REQUIRE( js.messageCount( "ORDERS" ) == 5 );
                }
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

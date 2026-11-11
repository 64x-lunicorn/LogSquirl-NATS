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
 * @file natsserver.cpp
 * @brief Starts and stops the nats-server the test build provides.
 *
 * The server picks its own free port (-p -1) and writes it to a ports file
 * (--ports_file_dir), so two tests never race for a port.  It is ready once
 * that file names the port and a TCP connection to it succeeds.
 */

#include "natsserver.h"

#include "testconfig.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStringList>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>
#include <QUrl>

#include <atomic>
#include <stdexcept>

namespace nats_test {

namespace {

std::atomic<int> g_skipped{ 0 };

constexpr int kStartTimeoutMs = 15000;

/// The port of the client listener in a ports file, or 0.
int portFromPortsFile( const QString& path )
{
    QFile file( path );
    if ( !file.open( QIODevice::ReadOnly ) ) {
        return 0;
    }
    const auto urls = QJsonDocument::fromJson( file.readAll() ).object().value( "nats" ).toArray();
    if ( urls.isEmpty() ) {
        return 0;
    }
    return QUrl( urls.first().toString() ).port( 0 );
}

bool accepts( int port )
{
    QTcpSocket socket;
    socket.connectToHost( QStringLiteral( "127.0.0.1" ), static_cast<quint16>( port ) );
    return socket.waitForConnected( 500 );
}

} // namespace

void markSkipped()
{
    ++g_skipped;
}

int skippedTests()
{
    return g_skipped.load();
}

bool runningInCi()
{
    const QString ci = qEnvironmentVariable( "CI" ).trimmed().toLower();
    return !ci.isEmpty() && ci != "0" && ci != "false" && ci != "no" && ci != "off";
}

struct NatsServer::Private {
    QProcess process;
    QTemporaryDir dir; ///< Ports file and JetStream store.
    int port = 0;

    QString output()
    {
        return QString::fromUtf8( process.readAll() );
    }

    void stop()
    {
        if ( process.state() == QProcess::NotRunning ) {
            return;
        }
#ifdef Q_OS_WIN
        // A console program does not see the WM_CLOSE terminate() sends.
        process.kill();
#else
        process.terminate(); // SIGTERM: the server shuts down cleanly.
        if ( process.waitForFinished( 5000 ) ) {
            return;
        }
        process.kill();
#endif
        process.waitForFinished( 5000 );
    }
};

QString NatsServer::unavailableReason()
{
    const QString reason = QStringLiteral( LOGSQUIRL_NATS_TEST_SERVER_UNAVAILABLE );
    if ( !reason.isEmpty() ) {
        return reason;
    }
    const QString executable = QStringLiteral( LOGSQUIRL_NATS_TEST_SERVER );
    if ( executable.isEmpty() || !QFile::exists( executable ) ) {
        return QStringLiteral( "the nats-server binary %1 is missing; configure again" )
            .arg( executable );
    }
    return {};
}

NatsServer::NatsServer( const NatsServerOptions& options )
    : d( std::make_unique<Private>() )
{
    if ( !available() ) {
        throw std::runtime_error( "no nats-server: " + unavailableReason().toStdString() );
    }
    if ( !d->dir.isValid() ) {
        throw std::runtime_error( "cannot create a temporary directory for nats-server" );
    }

    // The configuration file first: the command line overrides what it says
    // about the address and port.
    QStringList arguments;
    if ( !options.configFile.isEmpty() ) {
        arguments << "-c" << options.configFile;
    }
    arguments << "-a" << "127.0.0.1" << "-p" << "-1" << "--ports_file_dir" << d->dir.path();
    if ( options.jetStream ) {
        arguments << "-js" << "-sd" << d->dir.filePath( "jetstream" );
    }

    d->process.setProcessChannelMode( QProcess::MergedChannels );
    d->process.start( QStringLiteral( LOGSQUIRL_NATS_TEST_SERVER ), arguments );
    if ( !d->process.waitForStarted( kStartTimeoutMs ) ) {
        throw std::runtime_error( "nats-server did not start: "
                                  + d->process.errorString().toStdString() );
    }

    QElapsedTimer timer;
    timer.start();
    while ( d->port == 0 || !accepts( d->port ) ) {
        if ( d->process.state() == QProcess::NotRunning ) {
            throw std::runtime_error( "nats-server exited:\n" + d->output().toStdString() );
        }
        if ( timer.elapsed() > kStartTimeoutMs ) {
            d->stop();
            throw std::runtime_error( "nats-server did not accept connections in time:\n"
                                      + d->output().toStdString() );
        }
        if ( d->port == 0 ) {
            const auto portsFiles
                = QDir( d->dir.path() ).entryList( { QStringLiteral( "*.ports" ) }, QDir::Files );
            if ( !portsFiles.isEmpty() ) {
                d->port = portFromPortsFile( d->dir.filePath( portsFiles.first() ) );
            }
        }
        d->process.waitForReadyRead( 20 ); // also keeps the output pipe drained
    }
}

NatsServer::~NatsServer()
{
    d->stop();
}

int NatsServer::port() const
{
    return d->port;
}

QString NatsServer::url() const
{
    return QStringLiteral( "nats://127.0.0.1:%1" ).arg( d->port );
}

} // namespace nats_test

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
 * @file tcpproxy.h
 * @brief A TCP proxy in front of a test's nats-server, to cut a session's
 *        connection while the server runs on.
 *
 * It forwards each connection to the server's port, in a thread of its
 * own, so that a test thread blocked in stop() does not hold it up.
 * cut() closes the connections as a network failure would; refuse()
 * stops it from accepting new ones, so a client cannot reconnect.
 */

#pragma once

#include <QHostAddress>
#include <QList>
#include <QObject>
#include <QString>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>

#include <stdexcept>

namespace nats_test {

class TcpProxy {
public:
    /// Listen on a free port of 127.0.0.1, forwarding to @p upstreamPort.
    explicit TcpProxy( int upstreamPort )
        : upstreamPort_( upstreamPort )
    {
        worker_.moveToThread( &thread_ );
        thread_.start();
        run( [ this ] {
            server_ = new QTcpServer( &worker_ );
            QObject::connect( server_, &QTcpServer::newConnection, &worker_,
                              [ this ] { acceptPending(); } );
            if ( server_->listen( QHostAddress::LocalHost ) ) {
                port_ = server_->serverPort();
            }
        } );
        if ( port_ == 0 ) {
            throw std::runtime_error( "the proxy could not listen" );
        }
    }

    ~TcpProxy()
    {
        run( [ this ] {
            cutNow();
            delete server_;
        } );
        thread_.quit();
        thread_.wait();
    }

    TcpProxy( const TcpProxy& ) = delete;
    TcpProxy& operator=( const TcpProxy& ) = delete;

    /// Its client URL, nats://127.0.0.1:<port>.
    QString url() const
    {
        return QStringLiteral( "nats://127.0.0.1:%1" ).arg( port_ );
    }

    /// Close every connection, as a broken network would; new ones are
    /// accepted unless refuse() was called.
    void cut()
    {
        run( [ this ] { cutNow(); } );
    }

    /// Accept no new connection from now on.
    void refuse()
    {
        run( [ this ] { server_->close(); } );
    }

private:
    template <typename Function>
    void run( Function function )
    {
        QMetaObject::invokeMethod( &worker_, function, Qt::BlockingQueuedConnection );
    }

    void acceptPending()
    {
        while ( auto* client = server_->nextPendingConnection() ) {
            auto* upstream = new QTcpSocket( &worker_ );
            upstream->connectToHost( QHostAddress::LocalHost,
                                     static_cast<quint16>( upstreamPort_ ) );
            if ( !upstream->waitForConnected( 3000 ) ) {
                delete upstream;
                delete client;
                continue;
            }
            client->setParent( &worker_ );
            const auto forward = []( QTcpSocket* from, QTcpSocket* to ) {
                QObject::connect( from, &QTcpSocket::readyRead, to,
                                  [ from, to ] { to->write( from->readAll() ); } );
                QObject::connect( from, &QTcpSocket::disconnected, to,
                                  [ to ] { to->disconnectFromHost(); } );
            };
            forward( client, upstream );
            forward( upstream, client );
            sockets_ << client << upstream;
            if ( client->bytesAvailable() > 0 ) {
                upstream->write( client->readAll() );
            }
        }
    }

    void cutNow()
    {
        const auto sockets = sockets_;
        sockets_.clear();
        for ( auto* socket : sockets ) {
            socket->disconnect();
            socket->abort();
            delete socket;
        }
    }

    const int upstreamPort_;
    QThread thread_;
    QObject worker_;
    QTcpServer* server_ = nullptr;
    QList<QTcpSocket*> sockets_;
    int port_ = 0;
};

} // namespace nats_test

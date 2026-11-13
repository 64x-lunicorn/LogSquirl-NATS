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
 * holdRoundTrip() holds back a session's round trip that confirms its
 * subscriptions, so a test publishes while it is under way.
 */

#pragma once

#include <QByteArray>
#include <QHostAddress>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>

#include <algorithm>
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

    /// Hold back what the next client sends from its first PING after a
    /// SUB on, until releaseRoundTrip(): the round trip that confirms a
    /// session's subscriptions waits, while the server already delivers to
    /// them.  For one client.
    void holdRoundTrip()
    {
        run( [ this ] {
            holdArmed_ = true;
            seen_.clear();
        } );
    }

    /// Whether the PING of that round trip is held back: the server has the
    /// subscriptions, and has not answered.
    bool holdingRoundTrip()
    {
        bool holding = false;
        run( [ this, &holding ] { holding = holding_; } );
        return holding;
    }

    /// Send what was held back, and forward everything as before.
    void releaseRoundTrip()
    {
        run( [ this ] {
            if ( heldTo_ ) {
                heldTo_->write( held_ );
            }
            held_.clear();
            holding_ = false;
            holdArmed_ = false;
        } );
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
            QObject::connect( client, &QTcpSocket::readyRead, upstream,
                              [ this, client, upstream ] { fromClient( client, upstream ); } );
            QObject::connect( client, &QTcpSocket::disconnected, upstream,
                              [ upstream ] { upstream->disconnectFromHost(); } );
            forward( upstream, client );
            sockets_ << client << upstream;
            if ( client->bytesAvailable() > 0 ) {
                fromClient( client, upstream );
            }
        }
    }

    /// Forward what @p client sent to @p upstream, holding back the round
    /// trip once holdRoundTrip() asked for it.
    void fromClient( QTcpSocket* client, QTcpSocket* upstream )
    {
        const auto data = client->readAll();
        if ( holding_ ) {
            held_ += data;
            return;
        }
        if ( !holdArmed_ ) {
            upstream->write( data );
            return;
        }
        const auto before = seen_.size();
        seen_ += data;
        const auto sub = seen_.indexOf( "SUB " );
        const auto ping = sub < 0 ? -1 : seen_.indexOf( "PING\r\n", sub );
        if ( ping < 0 ) {
            upstream->write( data );
            return;
        }
        // Part of the PING may be forwarded already: the server waits for
        // the rest of its line.
        const auto at = std::max<qsizetype>( 0, ping - before );
        upstream->write( data.left( at ) );
        held_ = data.mid( at );
        heldTo_ = upstream;
        holding_ = true;
    }

    void cutNow()
    {
        const auto sockets = sockets_;
        sockets_.clear();
        // What was held back goes with the connection.
        held_.clear();
        holding_ = false;
        holdArmed_ = false;
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
    // holdRoundTrip(): on the proxy's thread.
    bool holdArmed_ = false;      ///< Hold back from the next PING after a SUB.
    bool holding_ = false;        ///< Holding back what the client sends.
    QByteArray seen_;             ///< What the client sent since armed, to find them in.
    QByteArray held_;             ///< What is held back.
    QPointer<QTcpSocket> heldTo_; ///< Where it goes.
};

} // namespace nats_test

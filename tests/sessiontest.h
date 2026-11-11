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
 * @file sessiontest.h
 * @brief What tests of a capture session use: waiting, reading the
 *        session's file, and publishing with nats.c.
 */

#pragma once

// First: on Windows nats.h includes winsock2.h, which must come before any
// windows.h.
#include <nats.h>

#include <catch2/catch.hpp>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QString>
#include <QStringList>
#include <QThread>

#include <functional>
#include <utility>
#include <vector>

namespace nats_test {

/// Run the event loop until @p condition holds or @p timeoutMs passed;
/// whether it holds.
template <typename Condition>
bool waitFor( Condition condition, int timeoutMs = 3000 )
{
    QElapsedTimer timer;
    timer.start();
    while ( !condition() ) {
        if ( timer.elapsed() > timeoutMs ) {
            return condition();
        }
        QCoreApplication::processEvents( QEventLoop::AllEvents, 10 );
        QThread::msleep( 5 );
    }
    return true;
}

/// Run the event loop for @p ms milliseconds.
inline void settle( int ms )
{
    waitFor( [] { return false; }, ms );
}

/// The lines of the file at @p path (UTF-8), without terminators; empty if
/// it cannot be read.
inline QStringList readLines( const QString& path )
{
    QFile file( path );
    if ( !file.open( QIODevice::ReadOnly ) ) {
        return {};
    }
    QStringList lines;
    while ( !file.atEnd() ) {
        auto line = file.readLine();
        if ( !line.endsWith( '\n' ) ) {
            break; // a line still being written
        }
        line.chop( 1 );
        lines << QString::fromUtf8( line );
    }
    return lines;
}

/// A nats.c connection that publishes test messages.
class Publisher {
public:
    /// Connect to @p url; @p configure sets further options (credentials,
    /// TLS) before it connects.
    explicit Publisher( const QString& url,
                        const std::function<void( natsOptions* )>& configure = {} )
    {
        natsOptions* options = nullptr;
        check( natsOptions_Create( &options ), "natsOptions_Create" );
        natsOptions_SetURL( options, url.toUtf8().constData() );
        if ( configure ) {
            configure( options );
        }
        natsOptions_SetTimeout( options, 5000 );
        const auto status = natsConnection_Connect( &connection_, options );
        natsOptions_Destroy( options );
        check( status, "natsConnection_Connect" );
    }

    ~Publisher()
    {
        natsConnection_Destroy( connection_ );
    }

    Publisher( const Publisher& ) = delete;
    Publisher& operator=( const Publisher& ) = delete;

    /// Publish @p payload on @p subject, with a reply subject and headers if
    /// given; with @p flush, wait until the server has it.
    void publish( const char* subject, const QByteArray& payload, const char* reply = nullptr,
                  const std::vector<std::pair<const char*, const char*>>& headers = {},
                  bool flush = true )
    {
        natsMsg* message = nullptr;
        check( natsMsg_Create( &message, subject, reply, payload.constData(),
                               static_cast<int>( payload.size() ) ),
               "natsMsg_Create" );
        for ( const auto& [ key, value ] : headers ) {
            natsMsgHeader_Add( message, key, value );
        }
        const auto status = natsConnection_PublishMsg( connection_, message );
        natsMsg_Destroy( message );
        check( status, "natsConnection_PublishMsg" );
        if ( flush ) {
            check( natsConnection_Flush( connection_ ), "natsConnection_Flush" );
        }
    }

private:
    static void check( natsStatus status, const char* what )
    {
        if ( status != NATS_OK ) {
            const char* detail = nats_GetLastError( nullptr );
            FAIL( what << ": " << natsStatus_GetText( status ) << " " << ( detail ? detail : "" ) );
        }
    }

    natsConnection* connection_ = nullptr;
};

} // namespace nats_test

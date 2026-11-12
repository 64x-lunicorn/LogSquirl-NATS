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

    natsConnection* connection() const
    {
        return connection_;
    }

    /// Fail the test if @p status is not NATS_OK, saying what failed.
    static void check( natsStatus status, const char* what )
    {
        if ( status != NATS_OK ) {
            const char* detail = nats_GetLastError( nullptr );
            FAIL( what << ": " << natsStatus_GetText( status ) << " " << ( detail ? detail : "" ) );
        }
    }

private:
    natsConnection* connection_ = nullptr;
};

/// JetStream on the connection of a Publisher: streams to replay, filled
/// with messages, and what the server says about them.
class JetStream {
public:
    explicit JetStream( Publisher& publisher )
    {
        jsOptions options;
        jsOptions_Init( &options );
        // Unbounded, a burst overruns the server's ingest queue, which then
        // refuses messages.
        options.PublishAsync.MaxPending = 1000;
        Publisher::check( natsConnection_JetStream( &js_, publisher.connection(), &options ),
                          "natsConnection_JetStream" );
    }

    ~JetStream()
    {
        jsCtx_Destroy( js_ );
    }

    JetStream( const JetStream& ) = delete;
    JetStream& operator=( const JetStream& ) = delete;

    /// Add the stream @p name, in memory, on @p subjects.
    void addStream( const char* name, const std::vector<const char*>& subjects )
    {
        jsStreamConfig config;
        jsStreamConfig_Init( &config );
        config.Name = name;
        config.Subjects = const_cast<const char**>( subjects.data() );
        config.SubjectsLen = static_cast<int>( subjects.size() );
        config.Storage = js_MemoryStorage;
        jsErrCode error = static_cast<jsErrCode>( 0 );
        jsStreamInfo* info = nullptr;
        Publisher::check( js_AddStream( &info, js_, &config, nullptr, &error ), "js_AddStream" );
        jsStreamInfo_Destroy( info );
    }

    /// Publish @p payload on @p subject into its stream; the sequence the
    /// stream stored it at.
    quint64 publish( const char* subject, const QByteArray& payload,
                     const std::vector<std::pair<const char*, const char*>>& headers = {} )
    {
        natsMsg* message = nullptr;
        Publisher::check( natsMsg_Create( &message, subject, nullptr, payload.constData(),
                                          static_cast<int>( payload.size() ) ),
                          "natsMsg_Create" );
        for ( const auto& [ key, value ] : headers ) {
            natsMsgHeader_Add( message, key, value );
        }
        jsPubAck* ack = nullptr;
        const auto status = js_PublishMsg( &ack, js_, message, nullptr, nullptr );
        natsMsg_Destroy( message );
        Publisher::check( status, "js_PublishMsg" );
        const auto sequence = ack->Sequence;
        jsPubAck_Destroy( ack );
        return sequence;
    }

    /// Publish @p count messages "1", "2", … on @p subject into the stream
    /// @p name, without waiting for each acknowledgment, and check that it
    /// holds them.
    void publishMany( const char* name, const char* subject, int count )
    {
        const auto before = messageCount( name );
        for ( int i = 1; i <= count; ++i ) {
            const auto payload = QByteArray::number( i );
            Publisher::check( js_PublishAsync( js_, subject, payload.constData(),
                                               static_cast<int>( payload.size() ), nullptr ),
                              "js_PublishAsync" );
        }
        jsPubOptions options;
        jsPubOptions_Init( &options );
        options.MaxWait = 60000;
        Publisher::check( js_PublishAsyncComplete( js_, &options ), "js_PublishAsyncComplete" );
        REQUIRE( messageCount( name ) == before + count );
    }

    /// How many messages the stream @p name holds.
    quint64 messageCount( const char* name )
    {
        jsStreamInfo* info = nullptr;
        Publisher::check( js_GetStreamInfo( &info, js_, name, nullptr, nullptr ),
                          "js_GetStreamInfo" );
        const auto count = info->State.Msgs;
        jsStreamInfo_Destroy( info );
        return count;
    }

    /// The time the stream @p name stored its message @p sequence, in
    /// microseconds since the epoch (UTC).
    qint64 storedUs( const char* name, quint64 sequence )
    {
        natsMsg* message = nullptr;
        Publisher::check( js_GetMsg( &message, js_, name, sequence, nullptr, nullptr ),
                          "js_GetMsg" );
        const auto ns = natsMsg_GetTime( message );
        natsMsg_Destroy( message );
        return ns / 1000;
    }

    jsCtx* context() const
    {
        return js_;
    }

private:
    jsCtx* js_ = nullptr;
};

} // namespace nats_test

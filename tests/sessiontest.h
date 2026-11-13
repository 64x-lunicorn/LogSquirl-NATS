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
 * @brief What tests of a capture session use: its configuration, its
 *        transitions, waiting, reading the session's file, a file whose
 *        writes fail, and publishing with nats.c.
 *
 * The lines read here are read into fields by the line reader, capture.h.
 */

#pragma once

// First: on Windows nats.h includes winsock2.h, which must come before any
// windows.h.
#include <nats.h>

#include <catch2/catch.hpp>

#include "natssession.h"
#include "sessionconfig.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QStringList>
#include <QThread>

#include <atomic>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace nats_test {

/// The temporary root a test's NatsSession is given to write below: the
/// system's, as the plugin's default.
inline QString sessionTempRoot()
{
    return QDir::tempPath();
}

/// The configuration of a session on @p url that subscribes to each of
/// @p subjects, in no queue group, the other settings the defaults; with no
/// subject, it is to be given its subscriptions or a replay.
inline nats_monitor::SessionConfig sessionConfig( const QString& url,
                                                  const QStringList& subjects = {} )
{
    nats_monitor::SessionConfig config;
    config.url = url;
    for ( const auto& subject : subjects ) {
        config.subscriptions << nats_monitor::Subscription{ subject, {} };
    }
    return config;
}

/// The configuration of a session on @p url that replays @p stream from its
/// start (ReplayConfig's defaults).
inline nats_monitor::SessionConfig replayConfig( const QString& url, const QString& stream )
{
    auto config = sessionConfig( url );
    config.replay.stream = stream;
    return config;
}

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

/// The transitions a session reports, in order, from when it is made.
class Transitions {
public:
    using Kind = nats_monitor::Transition::Kind;

    explicit Transitions( nats_monitor::NatsSession& session )
    {
        QObject::connect( &session, &nats_monitor::NatsSession::transitioned,
                          [ list = list_ ]( const nats_monitor::Transition& transition ) {
                              list->append( transition );
                          } );
    }

    const QList<nats_monitor::Transition>& all() const
    {
        return *list_;
    }

    /// Their kinds, in order.
    QList<Kind> kinds() const
    {
        QList<Kind> kinds;
        for ( const auto& transition : *list_ ) {
            kinds << transition.kind;
        }
        return kinds;
    }

    /// How many of @p kind.
    int count( Kind kind ) const
    {
        return static_cast<int>( kinds().count( kind ) );
    }

    /// The last of @p kind; a default one if none.
    nats_monitor::Transition last( Kind kind ) const
    {
        for ( auto it = list_->crbegin(); it != list_->crend(); ++it ) {
            if ( it->kind == kind ) {
                return *it;
            }
        }
        return {};
    }

private:
    std::shared_ptr<QList<nats_monitor::Transition>> list_
        = std::make_shared<QList<nats_monitor::Transition>>();
};

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

/// The files of the capture whose first file is @p first, in order:
/// nats_<name>.log, nats_<name>.2.log, nats_<name>.3.log, …
inline QStringList captureFiles( const QString& first )
{
    QStringList files;
    if ( !QFileInfo( first ).isFile() ) {
        return files;
    }
    files << first;
    auto stem = first;
    stem.chop( 4 ); // ".log"
    for ( int n = 2;; ++n ) {
        const auto next = QStringLiteral( "%1.%2.log" ).arg( stem ).arg( n );
        if ( !QFileInfo( next ).isFile() ) {
            return files;
        }
        files << next;
    }
}

/// A file whose writes fail once @p failing is set: a full disk, a volume
/// gone.  A capture sink's, or a session's, is given it to open its files.
class FailingFile : public QFile {
public:
    FailingFile( const QString& path, const std::shared_ptr<std::atomic<bool>>& failing )
        : QFile( path )
        , failing_( failing )
    {
    }

protected:
    qint64 writeData( const char* data, qint64 length ) override
    {
        if ( failing_->load() ) {
            setErrorString( QStringLiteral( "No space left on device" ) );
            return -1;
        }
        return QFile::writeData( data, length );
    }

private:
    std::shared_ptr<std::atomic<bool>> failing_;
};

/// Opens the files of a capture as FailingFiles, failing once @p failing
/// is set.
inline auto failingFiles( const std::shared_ptr<std::atomic<bool>>& failing )
{
    return [ failing ]( const QString& path ) -> std::unique_ptr<QFile> {
        return std::make_unique<FailingFile>( path, failing );
    };
}

/// The size of the file at @p path in bytes; 0 if there is none.
inline qint64 sizeOf( const QString& path )
{
    return QFileInfo( path ).size();
}

/// Whether the file at @p path is empty or ends with a line terminator.
inline bool endsWithNewline( const QString& path )
{
    QFile file( path );
    if ( !file.open( QIODevice::ReadOnly ) ) {
        return false;
    }
    const auto content = file.readAll();
    return content.isEmpty() || content.endsWith( '\n' );
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
        // With that many acks outstanding, a publish waits for one: nats.c's
        // default of 200 ms is too short for a loaded machine, and the wait
        // ends as soon as an ack arrives, so an idle one is as fast as before.
        options.PublishAsync.StallWait = 60000;
        Publisher::check( natsConnection_JetStream( &js_, publisher.connection(), &options ),
                          "natsConnection_JetStream" );
    }

    ~JetStream()
    {
        jsCtx_Destroy( js_ );
    }

    JetStream( const JetStream& ) = delete;
    JetStream& operator=( const JetStream& ) = delete;

    /// Add the stream @p name, in memory, on @p subjects, with @p retention.
    void addStream( const char* name, const std::vector<const char*>& subjects,
                    jsRetentionPolicy retention = js_LimitsPolicy )
    {
        jsStreamConfig config;
        jsStreamConfig_Init( &config );
        config.Name = name;
        config.Subjects = const_cast<const char**>( subjects.data() );
        config.SubjectsLen = static_cast<int>( subjects.size() );
        config.Storage = js_MemoryStorage;
        config.Retention = retention;
        jsErrCode error = static_cast<jsErrCode>( 0 );
        jsStreamInfo* info = nullptr;
        Publisher::check( js_AddStream( &info, js_, &config, nullptr, &error ), "js_AddStream" );
        jsStreamInfo_Destroy( info );
    }

    /// Add the durable pull consumer @p durable to the stream @p name; it
    /// acknowledges explicitly, and is never read here.
    void addDurable( const char* name, const char* durable )
    {
        jsConsumerConfig config;
        jsConsumerConfig_Init( &config );
        config.Durable = durable;
        config.AckPolicy = js_AckExplicit;
        jsConsumerInfo* info = nullptr;
        Publisher::check( js_AddConsumer( &info, js_, name, &config, nullptr, nullptr ),
                          "js_AddConsumer" );
        jsConsumerInfo_Destroy( info );
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

    /// The names of the consumers of the stream @p name, sorted.
    QStringList consumers( const char* name )
    {
        jsConsumerNamesList* list = nullptr;
        const auto status = js_ConsumerNames( &list, js_, name, nullptr, nullptr );
        if ( status == NATS_NOT_FOUND ) {
            return {}; // nats.c's answer for none
        }
        Publisher::check( status, "js_ConsumerNames" );
        QStringList names;
        for ( int i = 0; list && i < list->Count; ++i ) {
            names << QString::fromUtf8( list->List[ i ] );
        }
        jsConsumerNamesList_Destroy( list );
        names.sort();
        return names;
    }

    jsCtx* context() const
    {
        return js_;
    }

private:
    jsCtx* js_ = nullptr;
};

} // namespace nats_test

/// A transition as a failed assertion shows it: {kind, "detail", "server"}.
template <>
struct Catch::StringMaker<nats_monitor::Transition> {
    static std::string convert( const nats_monitor::Transition& transition )
    {
        return "{" + std::to_string( static_cast<int>( transition.kind ) ) + ", \""
               + transition.detail.toStdString() + "\", \"" + transition.server.toStdString()
               + "\"}";
    }
};

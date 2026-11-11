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
 * @file natssession.cpp
 * @brief Implementation of NatsSession: nats.c callbacks, queue, writer thread.
 *
 * HOW IT WORKS
 * ────────────
 *   1. start() creates the file and starts the writer thread.
 *   2. The writer thread connects and subscribes; its "connected" marker
 *      is the capture's first line.
 *   3. nats.c's callbacks format a line and push it onto the queue under a
 *      mutex, and wake the writer.  The writer swaps the whole queue out,
 *      writes the batch and flushes, so it never holds the mutex while
 *      writing.
 *   4. stop() asks the writer to end: it unsubscribes, closes the
 *      connection, waits until nats.c has made its last callback, writes
 *      what is left and a "stopped" marker, and closes the file.
 *
 * What the callbacks use (Shared) is apart from the writer: should nats.c
 * not confirm within the timeout that its callbacks are over, Shared is
 * leaked rather than freed under a running callback.
 */

// First: on Windows nats.h includes winsock2.h, which must come before any
// windows.h.
#include <nats.h>

#include "natssession.h"

#include "tempdirs.h"

#include <QFile>
#include <QMetaObject>
#include <QRegularExpression>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace nats_monitor {

namespace {

/// How long stopping waits for nats.c to confirm its last callback.
constexpr auto kCallbackTimeout = std::chrono::seconds( 5 );

/// What the writer reports to the session along with a line.
enum class Event {
    None,        ///< A message: nothing to report.
    Connected,   ///< Subscribed; the text is the server URL.
    Reconnected, ///< Connected again, the subscription restored; the text is the server URL.
    Status,      ///< A connection event.
    Error,       ///< Something failed.
};

struct Item {
    QByteArray line; ///< With its terminator.
    Event event = Event::None;
    QString text; ///< For the session's signals.
};

/// nats.c's text for @p status, with @p detail if there is one.
QString describe( natsStatus status, const char* detail )
{
    QString text = QString::fromUtf8( natsStatus_GetText( status ) );
    if ( detail && *detail ) {
        text += QStringLiteral( " (%1)" ).arg( QString::fromUtf8( detail ) );
    }
    return text;
}

/// describe() with this thread's last nats.c error.
QString lastError( natsStatus status )
{
    return describe( status, nats_GetLastError( nullptr ) );
}

QString connectedUrl( natsConnection* connection )
{
    char buffer[ 512 ] = {};
    if ( natsConnection_GetConnectedUrl( connection, buffer, sizeof buffer ) != NATS_OK ) {
        return {};
    }
    return QString::fromUtf8( buffer );
}

/// Make a subject usable as part of a file name on every platform.
QString safeFileName( const QString& subject )
{
    auto name = subject;
    static const QRegularExpression unsafe( QStringLiteral( "[^a-zA-Z0-9._-]" ) );
    name.replace( unsafe, QStringLiteral( "_" ) );
    return name;
}

/// The message as the formatter takes it.  Subject, reply and payload are
/// views into @p msg, valid while it is.
Message toMessage( natsMsg* msg )
{
    Message message;
    message.receivedUs = nowUs();
    const char* subject = natsMsg_GetSubject( msg );
    message.subject = QByteArray::fromRawData( subject, qstrlen( subject ) );
    if ( const char* reply = natsMsg_GetReply( msg ) ) {
        message.reply = QByteArray::fromRawData( reply, qstrlen( reply ) );
    }
    message.payload
        = QByteArray::fromRawData( natsMsg_GetData( msg ), natsMsg_GetDataLength( msg ) );

    const char** keys = nullptr;
    int keyCount = 0;
    if ( natsMsgHeader_Keys( msg, &keys, &keyCount ) != NATS_OK ) {
        return message; // none
    }
    // nats.c keeps the headers in a hash table: sort them, so that the same
    // headers always read the same.
    std::vector<const char*> sorted( keys, keys + keyCount );
    std::free( static_cast<void*>( keys ) );
    std::sort( sorted.begin(), sorted.end(),
               []( const char* a, const char* b ) { return qstrcmp( a, b ) < 0; } );
    for ( const char* key : sorted ) {
        const char** values = nullptr;
        int valueCount = 0;
        if ( natsMsgHeader_Values( msg, key, &values, &valueCount ) != NATS_OK ) {
            continue;
        }
        for ( int i = 0; i < valueCount; ++i ) {
            message.headers.append( { QByteArray( key ), QByteArray( values[ i ] ) } );
        }
        std::free( static_cast<void*>( values ) );
    }
    return message;
}

/// What nats.c's callbacks and the writer share.
struct Shared {
    FormatOptions format;

    std::mutex mutex;
    std::condition_variable wake;
    std::vector<Item> queue; ///< Unbounded for now: bounding and counting drops is #4's.
    bool stopRequested = false;
    bool stopping = false;         ///< The writer closes the connection: no disconnect marker.
    bool closed = false;           ///< nats.c made the closed callback.
    bool subscriptionDone = false; ///< nats.c made the subscription's completion callback.

    /// Queue @p line (without terminator) and wake the writer.
    void push( QByteArray line, Event event = Event::None, const QString& text = {} )
    {
        line.append( '\n' );
        {
            std::lock_guard<std::mutex> lock( mutex );
            queue.push_back( { std::move( line ), event, text } );
        }
        wake.notify_all();
    }

    /// Queue a marker line saying @p text; the signals get @p signalText,
    /// or @p text.
    void pushMarker( const QString& text, Event event, const QString& signalText = {} )
    {
        push( formatMarker( nowUs(), text.toUtf8() ), event,
              signalText.isEmpty() ? text : signalText );
    }

    // ── nats.c callbacks, on nats.c's threads ────────────────────────

    static void onMessage( natsConnection*, natsSubscription*, natsMsg* msg, void* closure )
    {
        auto* self = static_cast<Shared*>( closure );
        auto line = formatLine( toMessage( msg ), self->format );
        natsMsg_Destroy( msg );
        self->push( std::move( line ) );
    }

    static void onSubscriptionComplete( void* closure )
    {
        auto* self = static_cast<Shared*>( closure );
        {
            std::lock_guard<std::mutex> lock( self->mutex );
            self->subscriptionDone = true;
        }
        self->wake.notify_all();
    }

    static void onDisconnected( natsConnection*, void* closure )
    {
        auto* self = static_cast<Shared*>( closure );
        {
            std::lock_guard<std::mutex> lock( self->mutex );
            if ( self->stopping ) {
                return;
            }
        }
        self->pushMarker( QStringLiteral( "disconnected" ), Event::Status );
    }

    static void onReconnected( natsConnection* connection, void* closure )
    {
        const auto server = connectedUrl( connection );
        static_cast<Shared*>( closure )->pushMarker(
            QStringLiteral( "reconnected to %1" ).arg( server ), Event::Reconnected, server );
    }

    static void onClosed( natsConnection*, void* closure )
    {
        auto* self = static_cast<Shared*>( closure );
        {
            std::lock_guard<std::mutex> lock( self->mutex );
            self->closed = true;
        }
        self->wake.notify_all();
    }

    static void onError( natsConnection* connection, natsSubscription*, natsStatus status,
                         void* closure )
    {
        const char* detail = nullptr;
        natsConnection_GetLastError( connection, &detail );
        static_cast<Shared*>( closure )->pushMarker(
            QStringLiteral( "error: %1" ).arg( describe( status, detail ) ), Event::Error );
    }
};

} // namespace

// ── The writer ──────────────────────────────────────────────────────────

struct NatsSession::Worker {
    NatsSession* const session; ///< Only posted to; it joins the thread before it goes.
    const SessionConfig config;
    const int generation;
    Shared* shared = new Shared; ///< Leaked if nats.c does not let go of it.
    /// Whether shared may be freed: nats.c confirmed its last callback (or
    /// never got it).  Written by the thread, read after it is joined.
    bool released = true;
    QFile file;
    bool writeFailed = false;
    std::thread thread;

    Worker( NatsSession* owner, const SessionConfig& sessionConfig, int startNumber )
        : session( owner )
        , config( sessionConfig )
        , generation( startNumber )
    {
        shared->format = config.format;
    }

    /// Stops the thread and waits for it.
    ~Worker()
    {
        if ( thread.joinable() ) {
            {
                std::lock_guard<std::mutex> lock( shared->mutex );
                shared->stopRequested = true;
            }
            shared->wake.notify_all();
            thread.join();
        }
        if ( released ) {
            delete shared;
        }
    }

    Worker( const Worker& ) = delete;
    Worker& operator=( const Worker& ) = delete;

    /// Emit a session signal on the GUI thread.  Safe from this thread: the
    /// session joins it before it is destroyed, and Qt drops a call still
    /// queued then.
    template <typename Emitter>
    void post( Emitter emitter )
    {
        auto* target = session;
        QMetaObject::invokeMethod(
            target, [ target, emitter ] { emitter( target ); }, Qt::QueuedConnection );
    }

    void report( const Item& item )
    {
        switch ( item.event ) {
        case Event::None:
            return;
        case Event::Connected:
            post( [ url = item.text ]( NatsSession* s ) {
                Q_EMIT s->connected( url );
                Q_EMIT s->statusChanged( QStringLiteral( "connected to %1" ).arg( url ) );
            } );
            return;
        case Event::Reconnected:
            post( [ url = item.text ]( NatsSession* s ) {
                Q_EMIT s->connected( url );
                Q_EMIT s->statusChanged( QStringLiteral( "reconnected to %1" ).arg( url ) );
            } );
            return;
        case Event::Status:
            post( [ text = item.text ]( NatsSession* s ) { Q_EMIT s->statusChanged( text ); } );
            return;
        case Event::Error:
            post( [ text = item.text ]( NatsSession* s ) {
                Q_EMIT s->statusChanged( text );
                Q_EMIT s->errorOccurred( text );
            } );
            return;
        }
    }

    /// Write and flush a batch, and report its events.
    void write( const std::vector<Item>& batch )
    {
        for ( const auto& item : batch ) {
            if ( !writeFailed && file.write( item.line ) != item.line.size() ) {
                fail();
            }
            report( item );
        }
        if ( !writeFailed && !batch.empty() && !file.flush() ) {
            fail();
        }
    }

    /// The file cannot be written (disk full, volume gone): say so once.
    void fail()
    {
        writeFailed = true;
        const auto message
            = QStringLiteral( "Cannot write to %1: %2" ).arg( file.fileName(), file.errorString() );
        post( [ message ]( NatsSession* s ) {
            Q_EMIT s->statusChanged( message );
            Q_EMIT s->errorOccurred( message );
        } );
    }

    /// Write what is queued.
    void drain()
    {
        std::vector<Item> batch;
        {
            std::lock_guard<std::mutex> lock( shared->mutex );
            batch.swap( shared->queue );
        }
        write( batch );
    }

    /// Wait until @p done holds for Shared, or the timeout passed.
    template <typename Done>
    bool waitUntil( Done done )
    {
        std::unique_lock<std::mutex> lock( shared->mutex );
        return shared->wake.wait_for( lock, kCallbackTimeout,
                                      [ this, &done ] { return done( *shared ); } );
    }

    /// Create the options with the callbacks.
    natsStatus createOptions( natsOptions** options, const QByteArray& url )
    {
        natsStatus status = natsOptions_Create( options );
        if ( status == NATS_OK ) {
            status = natsOptions_SetURL( *options, url.constData() );
        }
        if ( status == NATS_OK ) {
            // A monitor keeps trying for as long as it runs.
            status = natsOptions_SetMaxReconnect( *options, -1 );
        }
        if ( status == NATS_OK ) {
            status = natsOptions_SetDisconnectedCB( *options, &Shared::onDisconnected, shared );
        }
        if ( status == NATS_OK ) {
            status = natsOptions_SetReconnectedCB( *options, &Shared::onReconnected, shared );
        }
        if ( status == NATS_OK ) {
            status = natsOptions_SetClosedCB( *options, &Shared::onClosed, shared );
        }
        if ( status == NATS_OK ) {
            status = natsOptions_SetErrorHandler( *options, &Shared::onError, shared );
        }
        return status;
    }

    /// Connect and subscribe; false, after an error marker, if that failed.
    bool open( natsOptions** options, natsConnection** connection, natsSubscription** subscription )
    {
        const auto url = config.url.toUtf8();
        natsStatus status = createOptions( options, url );
        if ( status == NATS_OK ) {
            status = natsConnection_Connect( connection, *options );
        }
        if ( status != NATS_OK ) {
            shared->pushMarker( QStringLiteral( "connection to %1 failed: %2" )
                                    .arg( config.url, lastError( status ) ),
                                Event::Error );
            return false;
        }

        const auto subject = config.subject.toUtf8();
        status = natsConnection_Subscribe( subscription, *connection, subject.constData(),
                                           &Shared::onMessage, shared );
        if ( status == NATS_OK ) {
            status = natsSubscription_SetOnCompleteCB( *subscription,
                                                       &Shared::onSubscriptionComplete, shared );
        }
        if ( status == NATS_OK ) {
            // A round trip: once the server has answered, it has the
            // subscription, so the "connected" marker means that every
            // message published from then on is captured.
            status = natsConnection_Flush( *connection );
        }
        if ( status != NATS_OK ) {
            shared->pushMarker( QStringLiteral( "subscribing to %1 failed: %2" )
                                    .arg( config.subject, lastError( status ) ),
                                Event::Error );
            return false;
        }

        const auto server = connectedUrl( *connection );
        shared->pushMarker(
            QStringLiteral( "connected to %1, subscribed to %2" ).arg( server, config.subject ),
            Event::Connected, server );
        return true;
    }

    /// Write batches until stopped (true) or the connection is closed for
    /// good (false).
    bool capture()
    {
        for ( ;; ) {
            std::vector<Item> batch;
            bool stop = false;
            bool closed = false;
            {
                std::unique_lock<std::mutex> lock( shared->mutex );
                shared->wake.wait( lock, [ this ] {
                    return !shared->queue.empty() || shared->stopRequested || shared->closed;
                } );
                batch.swap( shared->queue );
                stop = shared->stopRequested;
                closed = shared->closed;
            }
            write( batch );
            if ( stop || closed ) {
                return stop;
            }
        }
    }

    /// Unsubscribe, close and destroy; whether nats.c confirmed that it
    /// makes no callback any more.
    bool close( natsConnection* connection, natsSubscription* subscription )
    {
        {
            std::lock_guard<std::mutex> lock( shared->mutex );
            shared->stopping = true;
        }
        bool confirmed = true;
        if ( subscription ) {
            natsSubscription_Unsubscribe( subscription );
            confirmed = waitUntil( []( const Shared& s ) { return s.subscriptionDone; } );
        }
        if ( connection ) {
            natsConnection_Close( connection );
            confirmed = waitUntil( []( const Shared& s ) { return s.closed; } ) && confirmed;
        }
        natsSubscription_Destroy( subscription );
        natsConnection_Destroy( connection );
        return confirmed;
    }

    void run()
    {
        natsOptions* options = nullptr;
        natsConnection* connection = nullptr;
        natsSubscription* subscription = nullptr;

        const bool opened = open( &options, &connection, &subscription );
        bool stopped = false;
        if ( opened ) {
            stopped = capture();
        }
        else {
            std::lock_guard<std::mutex> lock( shared->mutex );
            stopped = shared->stopRequested;
        }

        released = close( connection, subscription );
        natsOptions_Destroy( options );

        drain(); // what arrived before the subscription ended
        if ( stopped ) {
            shared->pushMarker( QStringLiteral( "stopped" ), Event::Status );
        }
        else if ( opened ) {
            shared->pushMarker( QStringLiteral( "connection closed" ), Event::Error );
        }
        drain();
        file.close();

        if ( !stopped ) {
            post( [ generation = generation ]( NatsSession* s ) {
                s->onWorkerFinished( generation );
            } );
        }
    }
};

// ── NatsSession ─────────────────────────────────────────────────────────

NatsSession::NatsSession( const SessionConfig& config, QObject* parent )
    : QObject( parent )
    , config_( config )
    , tempDir_( tempDirTemplate( tempRoot() ) )
{
}

NatsSession::~NatsSession()
{
    // The owner may be half-destroyed already: stopping must not signal it.
    blockSignals( true );
    stop();
}

bool NatsSession::start()
{
    if ( worker_ ) {
        return true;
    }

    const auto fail = [ this ]( const QString& message ) {
        Q_EMIT statusChanged( message );
        Q_EMIT errorOccurred( message );
        return false;
    };
    if ( config_.subject.trimmed().isEmpty() ) {
        return fail( QStringLiteral( "No subject to subscribe to." ) );
    }
    if ( !tempDir_.isValid() ) {
        return fail( QStringLiteral( "Cannot create a temporary directory: %1" )
                         .arg( tempDir_.errorString() ) );
    }

    filePath_ = tempDir_.filePath( "nats_" + safeFileName( config_.subject ) + ".log" );
    auto worker = std::make_unique<Worker>( this, config_, ++generation_ );
    worker->file.setFileName( filePath_ );
    if ( !worker->file.open( QIODevice::WriteOnly | QIODevice::Append ) ) {
        return fail(
            QStringLiteral( "Cannot create %1: %2" ).arg( filePath_, worker->file.errorString() ) );
    }
    auto* raw = worker.get();
    worker->thread = std::thread( [ raw ] { raw->run(); } );
    worker_ = std::move( worker );
    return true;
}

void NatsSession::stop()
{
    worker_.reset(); // asks the writer to stop, and waits for it
}

bool NatsSession::isRunning() const
{
    return worker_ != nullptr;
}

QString NatsSession::filePath() const
{
    return filePath_;
}

void NatsSession::preserveFile()
{
    tempDir_.setAutoRemove( false );
}

void NatsSession::onWorkerFinished( int generation )
{
    if ( !worker_ || generation != generation_ ) {
        return; // stopped meanwhile, or the report of an earlier start
    }
    worker_.reset(); // the thread has ended: joins at once
    Q_EMIT finished();
}

} // namespace nats_monitor

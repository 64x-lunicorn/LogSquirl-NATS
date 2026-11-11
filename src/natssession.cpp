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
 *      writing.  A full queue drops the message and counts it; the next
 *      message queued is preceded by the dropped= marker.  Each message
 *      callback (and the slow-consumer error, and the disconnected
 *      callback) also reads nats.c's dropped count and marks what is new.
 *   4. Before a line that would take the file over the size limit, the
 *      writer opens the next file (NewOnly, so no earlier file is ever
 *      truncated), closes the full one and reports the new path.
 *   5. stop() asks the writer to end.  It lifts the pending limits, so
 *      nats.c drops nothing more, and marks its last dropped count.  It
 *      drains the subscription (or, not connected, waits until nats.c
 *      holds nothing for it), writing all the while, closes the
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
#include <QFileInfo>
#include <QMetaObject>
#include <QRegularExpression>

#include <algorithm>
#include <atomic>
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
constexpr auto kCallbackTimeout = std::chrono::milliseconds( 5000 );

/// How long stopping lets nats.c drain the subscription: its flush may take
/// its own timeout of 10 s, if the connection breaks meanwhile.
constexpr auto kDrainTimeout = std::chrono::milliseconds( 15000 );

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

/// File @p number of a capture: <stem>.log, <stem>.2.log, <stem>.3.log, …
QString numberedPath( const QString& stem, int number )
{
    return number == 1 ? stem + QStringLiteral( ".log" )
                       : QStringLiteral( "%1.%2.log" ).arg( stem ).arg( number );
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
    /// What drops and messages are counted in, here and in NatsSession.
    using Count = quint64;

    FormatOptions format;

    std::mutex mutex;
    std::condition_variable wake;
    std::vector<Item> queue;
    int queueLimit = kDefaultQueueLimit; ///< Message lines the queue holds; markers always fit.
    int queuedMessages = 0;              ///< Message lines in the queue.
    Count queueFullUnmarked = 0;         ///< Dropped from a full queue, not marked yet.
    bool stopRequested = false;
    bool stopping = false;         ///< The writer closes the connection: no disconnect marker.
    bool closed = false;           ///< nats.c made the closed callback.
    bool subscriptionDone = false; ///< nats.c made the subscription's completion callback.
    /// nats.c's dropped count of the subscription that the markers cover;
    /// under the mutex.
    Count slowConsumerMarked = 0;

    std::atomic<Count> received{ 0 };    ///< Messages nats.c handed over.
    std::atomic<Count> dropped{ 0 };     ///< Drops counted: marked, or to be marked.
    std::atomic<int> limitMessages{ 0 }; ///< The subscription's pending limits, read back.
    std::atomic<int> limitBytes{ 0 };
    /// The subscription, for the disconnected callback; set once subscribed.
    std::atomic<natsSubscription*> subscription{ nullptr };

    /// The counts, each read on its own (see NatsSession::receivedCount()).
    NatsSession::Counts snapshot() const
    {
        return { received.load(), dropped.load(), { limitMessages.load(), limitBytes.load() } };
    }

    /// Queue @p line (without terminator); under the mutex.
    void enqueueLocked( QByteArray line, Event event = Event::None, const QString& text = {} )
    {
        line.append( '\n' );
        queue.push_back( { std::move( line ), event, text } );
    }

    /// Queue @p line (without terminator) and wake the writer.
    void push( QByteArray line, Event event = Event::None, const QString& text = {} )
    {
        {
            std::lock_guard<std::mutex> lock( mutex );
            enqueueLocked( std::move( line ), event, text );
        }
        wake.notify_all();
    }

    /// Queue the message line @p line (without terminator), or drop it if
    /// the queue is full.  The first message queued after drops is preceded
    /// by their marker.
    void pushMessage( QByteArray line )
    {
        {
            std::lock_guard<std::mutex> lock( mutex );
            if ( queuedMessages >= queueLimit ) {
                ++queueFullUnmarked;
                dropped.fetch_add( 1 );
                return;
            }
            markQueueFullLocked();
            enqueueLocked( std::move( line ) );
            ++queuedMessages;
        }
        wake.notify_all();
    }

    /// Queue the marker of the queue-full drops not marked yet; under the mutex.
    void markQueueFullLocked()
    {
        if ( queueFullUnmarked > 0 ) {
            pushDroppedLocked( queueFullUnmarked, "queue full" );
            queueFullUnmarked = 0;
        }
    }

    /// Take what is queued, under the mutex.
    std::vector<Item> takeLocked()
    {
        std::vector<Item> batch;
        batch.swap( queue );
        queuedMessages = 0;
        return batch;
    }

    /// Queue a dropped= marker for @p count drops for @p reason; under the mutex.
    void pushDroppedLocked( Count count, const char* reason )
    {
        enqueueLocked( formatMarker( nowUs(), "dropped=" + QByteArray::number( count ) + " ("
                                                  + reason + ")" ) );
    }

    /// Mark the slow-consumer drops of @p subscription not marked yet.
    void markSlowConsumer( natsSubscription* subscription )
    {
        int64_t total = 0;
        if ( natsSubscription_GetDropped( subscription, &total ) != NATS_OK ) {
            // Closed: nats.c no longer says.  It closes the subscription
            // when the connection closes, before it makes the disconnected
            // and closed callbacks, and discards what is still pending
            // (natsSub_close, dispatch.c).  So the count is read while it
            // still can be: before each message it delivers, at each slow
            // consumer error, when the connection breaks (still open then,
            // for the reconnect), and on stop.
            return;
        }
        const auto count = static_cast<Count>( total );
        {
            std::lock_guard<std::mutex> lock( mutex );
            if ( count <= slowConsumerMarked ) {
                return;
            }
            const auto unmarked = count - slowConsumerMarked;
            slowConsumerMarked = count;
            dropped.fetch_add( unmarked );
            pushDroppedLocked( unmarked, "slow consumer" );
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

    static void onMessage( natsConnection*, natsSubscription* subscription, natsMsg* msg,
                           void* closure )
    {
        auto* self = static_cast<Shared*>( closure );
        self->received.fetch_add( 1, std::memory_order_relaxed );
        // Drops since the last message: the gap is before this one.
        self->markSlowConsumer( subscription );
        auto line = formatLine( toMessage( msg ), self->format );
        natsMsg_Destroy( msg );
        self->pushMessage( std::move( line ) );
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
        // Nothing arrives until reconnected, so nothing is dropped: the
        // count now is the count of the gap, readable still should the
        // connection now be closed for good.  The writer destroys the
        // subscription only after the closed callback, which comes after
        // this one.
        if ( auto* subscription = self->subscription.load() ) {
            self->markSlowConsumer( subscription );
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

    static void onError( natsConnection* connection, natsSubscription* subscription,
                         natsStatus status, void* closure )
    {
        auto* self = static_cast<Shared*>( closure );
        if ( status == NATS_SLOW_CONSUMER && subscription ) {
            self->markSlowConsumer( subscription );
            return;
        }
        const char* detail = nullptr;
        natsConnection_GetLastError( connection, &detail );
        self->pushMarker( QStringLiteral( "error: %1" ).arg( describe( status, detail ) ),
                          Event::Error );
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
    /// The file being written; replaced at each rotation.
    std::unique_ptr<QFile> file = std::make_unique<QFile>();
    bool writeFailed = false;
    /// The first file's path without ".log" (numberedPath()).
    QString stem;
    int fileNumber = 1;   ///< Of the file being written: 1, 2, 3, …
    qint64 fileBytes = 0; ///< Written to the file being written.
    /// The size the file may reach before the next is opened: the limit, or
    /// one limit beyond where a rotation failed.
    qint64 rotateAt = 0;
    std::thread thread;

    /// Writes file number @p number of the files with the stem @p fileStem.
    Worker( NatsSession* owner, const SessionConfig& sessionConfig, int startNumber,
            const QString& fileStem, int number )
        : session( owner )
        , config( sessionConfig )
        , generation( startNumber )
        , stem( fileStem )
        , fileNumber( number )
        , rotateAt( sessionConfig.maxFileBytes )
    {
        shared->format = config.format;
        shared->queueLimit = config.queueLimit;
        file->setFileName( numberedPath( stem, fileNumber ) );
    }

    /// Stops the thread and waits for it.
    void join()
    {
        if ( thread.joinable() ) {
            {
                std::lock_guard<std::mutex> lock( shared->mutex );
                shared->stopRequested = true;
            }
            shared->wake.notify_all();
            thread.join();
        }
    }

    ~Worker()
    {
        join();
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
            if ( !writeFailed ) {
                rotateBefore( item.line.size() );
            }
            if ( !writeFailed ) {
                if ( file->write( item.line ) == item.line.size() ) {
                    fileBytes += item.line.size();
                }
                else {
                    fail();
                }
            }
            report( item );
        }
        if ( !writeFailed && !batch.empty() && !file->flush() ) {
            fail();
        }
    }

    /// Go on in the next file if a line of @p lineBytes would take this one
    /// over the size limit.  A line longer than the limit is written whole,
    /// into a file of its own: an empty file is never rotated.
    void rotateBefore( qint64 lineBytes )
    {
        if ( config.maxFileBytes <= 0 || fileBytes == 0 || fileBytes + lineBytes <= rotateAt ) {
            return;
        }
        const auto path = numberedPath( stem, fileNumber + 1 );
        auto next = std::make_unique<QFile>( path );
        if ( !next->open( QIODevice::WriteOnly | QIODevice::NewOnly ) ) {
            // Retried one limit later: asking on every line would report
            // the failure on every line.
            rotateAt = fileBytes + config.maxFileBytes;
            const auto message
                = QStringLiteral( "Cannot start the next file %1: %2; the capture goes on in %3." )
                      .arg( path, next->errorString(), file->fileName() );
            post( [ message ]( NatsSession* s ) {
                Q_EMIT s->statusChanged( message );
                Q_EMIT s->errorOccurred( message );
            } );
            return;
        }
        if ( !file->flush() ) {
            fail(); // says what was lost in the old file; the new one is fine
            writeFailed = false;
        }
        file->close();
        file = std::move( next );
        ++fileNumber;
        fileBytes = 0;
        rotateAt = config.maxFileBytes;
        post( [ generation = generation, path ]( NatsSession* s ) {
            s->onWorkerRotated( generation, path );
        } );
    }

    /// The file cannot be written (disk full, volume gone): say so once.
    void fail()
    {
        writeFailed = true;
        const auto message = QStringLiteral( "Cannot write to %1: %2" )
                                 .arg( file->fileName(), file->errorString() );
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
            batch = shared->takeLocked();
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
            shared->subscription = *subscription;
            status = natsSubscription_SetOnCompleteCB( *subscription,
                                                       &Shared::onSubscriptionComplete, shared );
        }
        if ( status == NATS_OK ) {
            status = natsSubscription_SetPendingLimits(
                *subscription, config.pendingLimits.messages, config.pendingLimits.bytes );
        }
        if ( status == NATS_OK ) {
            int messages = 0;
            int bytes = 0;
            status = natsSubscription_GetPendingLimits( *subscription, &messages, &bytes );
            shared->limitMessages = messages;
            shared->limitBytes = bytes;
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
                batch = shared->takeLocked();
                stop = shared->stopRequested;
                closed = shared->closed;
            }
            write( batch );
            if ( stop || closed ) {
                return stop;
            }
        }
    }

    /// Write what the callbacks queue until @p done (called without the
    /// mutex) holds, or @p timeout passed; whether it holds.
    template <typename Done>
    bool writeUntil( Done done, std::chrono::milliseconds timeout )
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        for ( ;; ) {
            const bool finished = done(); // before drain(): what came before is written
            drain();
            if ( finished ) {
                return true;
            }
            if ( std::chrono::steady_clock::now() >= deadline ) {
                return false;
            }
            std::unique_lock<std::mutex> lock( shared->mutex );
            shared->wake.wait_for( lock, std::chrono::milliseconds( 10 ),
                                   [ this ] { return !shared->queue.empty(); } );
        }
    }

    /// End the subscription without losing what nats.c still holds for it:
    /// nats.c discards the messages pending in a subscription once it is
    /// closed (dispatch.c), uncounted.
    void endSubscription( natsConnection* connection, natsSubscription* subscription )
    {
        // Lifted, nats.c drops nothing more (it decides under the lock that
        // SetPendingLimits takes), so the count read now is the last.
        natsSubscription_SetPendingLimits( subscription, -1, -1 );
        shared->markSlowConsumer( subscription );
        // Connected: drain.  nats.c sends UNSUB, waits for the server's
        // answer (so all that was sent before has arrived), delivers what
        // is pending and then makes the completion callback.  With no
        // timeout of its own (-1) it never closes the subscription early.
        if ( natsConnection_Status( connection ) == NATS_CONN_STATUS_CONNECTED
             && natsSubscription_DrainTimeout( subscription, -1 ) == NATS_OK ) {
            writeUntil(
                [ this ] {
                    std::lock_guard<std::mutex> lock( shared->mutex );
                    return shared->subscriptionDone;
                },
                kDrainTimeout );
            return;
        }
        // Not connected, nothing more arrives (and a drain would wait for
        // its flush to time out): wait until nats.c has delivered what it
        // holds.  A message it has taken from the queue it delivers even if
        // the subscription is closed meanwhile.
        writeUntil(
            [ subscription ] {
                int pending = 0;
                return natsSubscription_GetPending( subscription, &pending, nullptr ) != NATS_OK
                       || pending == 0;
            },
            kCallbackTimeout );
        natsSubscription_Unsubscribe( subscription );
    }

    /// End the subscription, close and destroy; whether nats.c confirmed
    /// that it makes no callback any more.
    bool close( natsConnection* connection, natsSubscription* subscription )
    {
        {
            std::lock_guard<std::mutex> lock( shared->mutex );
            shared->stopping = true;
        }
        if ( subscription ) {
            endSubscription( connection, subscription );
        }
        bool confirmed = true;
        if ( connection ) {
            natsConnection_Close( connection );
            confirmed = waitUntil( [ subscription ]( const Shared& s ) {
                return s.closed && ( !subscription || s.subscriptionDone );
            } );
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

        {
            // Drops no message after them marks yet.
            std::lock_guard<std::mutex> lock( shared->mutex );
            shared->markQueueFullLocked();
        }
        drain(); // what arrived before the subscription ended
        if ( stopped ) {
            shared->pushMarker( QStringLiteral( "stopped" ), Event::Status );
        }
        else if ( opened ) {
            shared->pushMarker( QStringLiteral( "connection closed" ), Event::Error );
        }
        drain();
        file->close();

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

    // Started again, the session goes on in its last file.
    const auto stem = tempDir_.filePath( "nats_" + safeFileName( config_.subject ) );
    int number = 1;
    while ( QFileInfo( numberedPath( stem, number + 1 ) ).isFile() ) {
        ++number;
    }
    filePath_ = numberedPath( stem, number );
    auto worker = std::make_unique<Worker>( this, config_, ++generation_, stem, number );
    if ( !worker->file->open( QIODevice::WriteOnly | QIODevice::Append ) ) {
        return fail( QStringLiteral( "Cannot create %1: %2" )
                         .arg( filePath_, worker->file->errorString() ) );
    }
    auto* raw = worker.get();
    worker->thread = std::thread( [ raw ] { raw->run(); } );
    worker_ = std::move( worker );
    return true;
}

void NatsSession::stop()
{
    releaseWorker();
}

void NatsSession::releaseWorker()
{
    if ( !worker_ ) {
        return;
    }
    worker_->join(); // asks the writer to stop, and waits for it
    counts_ = worker_->shared->snapshot();
    worker_.reset();
}

NatsSession::Counts NatsSession::counts() const
{
    return worker_ ? worker_->shared->snapshot() : counts_;
}

PendingLimits NatsSession::pendingLimits() const
{
    return counts().pendingLimits;
}

bool NatsSession::isRunning() const
{
    return worker_ != nullptr;
}

quint64 NatsSession::receivedCount() const
{
    return counts().received;
}

quint64 NatsSession::droppedCount() const
{
    return counts().dropped;
}

QString NatsSession::filePath() const
{
    return filePath_;
}

void NatsSession::preserveFile()
{
    tempDir_.setAutoRemove( false );
}

void NatsSession::onWorkerRotated( int generation, const QString& path )
{
    if ( generation != generation_ ) {
        return; // the report of an earlier start
    }
    filePath_ = path;
    Q_EMIT fileRotated( path );
}

void NatsSession::onWorkerFinished( int generation )
{
    if ( !worker_ || generation != generation_ ) {
        return; // stopped meanwhile, or the report of an earlier start
    }
    releaseWorker(); // the thread has ended: joins at once
    Q_EMIT finished();
}

} // namespace nats_monitor

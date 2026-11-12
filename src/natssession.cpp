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
 *   2. The writer thread connects and subscribes to each subject; its
 *      "connected" marker is the capture's first line.  A replay instead
 *      writes the marker and subscribes to the stream through nats.c's
 *      ordered consumer (js_Subscribe, jsSubOptions::Ordered); its
 *      callback takes the stored time and stream sequence from the
 *      message's metadata, and the consumer's pending count of 0 says it
 *      has caught up (or, before any message, the consumer's info).
 *   3. nats.c's callbacks format a line and push it onto the queue under a
 *      mutex, and wake the writer.  The writer swaps the whole queue out,
 *      writes the batch and flushes, so it never holds the mutex while
 *      writing.  A full queue drops the message and counts it; the next
 *      message queued is preceded by the dropped= marker.  Each message
 *      callback (and the slow-consumer error, and the disconnected
 *      callback) also reads nats.c's dropped count of its subscription
 *      (each, for the disconnected callback) and marks what is new.
 *   4. Before a line that would take the file over the size limit, the
 *      writer opens the next file (NewOnly, so no earlier file is ever
 *      truncated), closes the full one and reports the new path.
 *   5. stop() asks the writer to end.  It lifts the pending limits, so
 *      nats.c drops nothing more, and marks the last dropped counts.  It
 *      drains each subscription (or, not connected, waits until nats.c
 *      holds nothing for them), writing all the while, closes the
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

#include "natsoptions.h"
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

/// The URL @p connection is connected to, without its password
/// (nats.c's has the URL's user information).
QString connectedUrl( natsConnection* connection )
{
    char buffer[ 512 ] = {};
    if ( natsConnection_GetConnectedUrl( connection, buffer, sizeof buffer ) != NATS_OK ) {
        return {};
    }
    return redactUrls( QString::fromUtf8( buffer ) );
}

/// Make a subject usable as part of a file name on every platform.
QString safeFileName( const QString& subject )
{
    auto name = subject;
    static const QRegularExpression unsafe( QStringLiteral( "[^a-zA-Z0-9._-]" ) );
    name.replace( unsafe, QStringLiteral( "_" ) );
    return name;
}

/// How long the subjects' part of a file name may be: a file name may have
/// 255 bytes on every platform, and the path is longer still.
constexpr int kMaxFileNameSubjects = 100;

/// The subjects of @p subscriptions as the file's name: "orders.__billing._"
/// for orders.> and billing.*, cut to kMaxFileNameSubjects.
QString fileNameFor( const QList<Subscription>& subscriptions )
{
    QStringList subjects;
    for ( const auto& subscription : subscriptions ) {
        subjects << safeFileName( subscription.subject );
    }
    return subjects.join( '_' ).left( kMaxFileNameSubjects );
}

/// File @p number of a capture: <stem>.log, <stem>.2.log, <stem>.3.log, …
QString numberedPath( const QString& stem, int number )
{
    return number == 1 ? stem + QStringLiteral( ".log" )
                       : QStringLiteral( "%1.%2.log" ).arg( stem ).arg( number );
}

/// The numbered files of a capture, written one after the other: the file
/// being written, and when to go on in the next (natssession.h, ROTATION).
class RotatingFile {
public:
    /// What rotateBefore() did.
    struct Rotation {
        QString path;  ///< The next file, if the capture goes on there.
        QString error; ///< What failed, if anything.
    };

    /// The files <@p stem>.log, <@p stem>.2.log, … of at most @p maxBytes
    /// each; 0 for no limit.
    RotatingFile( const QString& stem, qint64 maxBytes )
        : stem_( stem )
        , maxBytes_( maxBytes )
        , rotateAt_( maxBytes )
    {
    }

    /// Open the last file there is, or the first: a session started again
    /// goes on in it, with what it holds counted against the limit.
    bool open()
    {
        while ( QFileInfo( numberedPath( stem_, number_ + 1 ) ).isFile() ) {
            ++number_;
        }
        file_->setFileName( numberedPath( stem_, number_ ) );
        if ( !file_->open( QIODevice::WriteOnly | QIODevice::Append ) ) {
            return false;
        }
        bytes_ = file_->size();
        return true;
    }

    /// Go on in the next file if a line of @p lineBytes would take this one
    /// over the size limit.  A line longer than the limit is written whole,
    /// into a file of its own: an empty file is never rotated.  Should the
    /// next file not open, the next try is one limit later: asking on every
    /// line would report the failure on every line.
    Rotation rotateBefore( qint64 lineBytes )
    {
        if ( maxBytes_ <= 0 || bytes_ == 0 || bytes_ + lineBytes <= rotateAt_ ) {
            return {};
        }
        const auto path = numberedPath( stem_, number_ + 1 );
        auto next = std::make_unique<QFile>( path );
        // NewOnly: an earlier file is never truncated.
        if ( !next->open( QIODevice::WriteOnly | QIODevice::NewOnly ) ) {
            rotateAt_ = bytes_ + maxBytes_;
            return { {},
                     QStringLiteral(
                         "Cannot start the next file %1: %2; the capture goes on in %3." )
                         .arg( path, next->errorString(), fileName() ) };
        }
        Rotation rotation{ path, {} };
        if ( !file_->flush() ) {
            rotation.error = writeError(); // lost in the old file; the new one is fine
        }
        file_->close();
        file_ = std::move( next );
        ++number_;
        bytes_ = 0;
        rotateAt_ = maxBytes_;
        return rotation;
    }

    /// Write @p line whole; false if it could not be.
    bool write( const QByteArray& line )
    {
        if ( file_->write( line ) != line.size() ) {
            return false;
        }
        bytes_ += line.size();
        return true;
    }

    bool flush()
    {
        return file_->flush();
    }

    void close()
    {
        file_->close();
    }

    /// The file being written.
    QString fileName() const
    {
        return file_->fileName();
    }

    QString errorString() const
    {
        return file_->errorString();
    }

    /// That the file being written cannot be written, and why.
    QString writeError() const
    {
        return QStringLiteral( "Cannot write to %1: %2" ).arg( fileName(), errorString() );
    }

private:
    const QString stem_;    ///< The first file's path without ".log".
    const qint64 maxBytes_; ///< 0: no limit.
    qint64 rotateAt_;       ///< The limit, or one limit beyond where a rotation failed.
    int number_ = 1;        ///< Of the file being written: 1, 2, 3, …
    qint64 bytes_ = 0;      ///< In the file being written.
    std::unique_ptr<QFile> file_ = std::make_unique<QFile>();
};

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

struct Shared;

/// What drops and messages are counted in, here and in NatsSession.
using Count = quint64;

/// One subscription of the session, as its callbacks see it.
struct SubscriptionState {
    Shared* shared = nullptr; ///< The session's, for the callbacks.
    /// Set once subscribed; for the disconnected and error callbacks.
    std::atomic<natsSubscription*> handle{ nullptr };
    /// nats.c's dropped count of the subscription that the markers cover;
    /// under Shared::mutex.
    Count slowConsumerMarked = 0;
    /// nats.c made the subscription's completion callback; under Shared::mutex.
    bool done = false;
    /// nats.c took the completion callback: done is awaited.  Only the
    /// writer reads and writes it.
    bool awaitsCompletion = false;
};

/// What nats.c's callbacks and the writer share.
struct Shared {
    FormatOptions format;

    /// One for each subscription of the config, made before connecting
    /// and never changed after: the callbacks read it without the mutex.
    std::vector<std::unique_ptr<SubscriptionState>> subscriptions;

    std::mutex mutex;
    std::condition_variable wake;
    std::vector<Item> queue;
    int queueLimit = kDefaultQueueLimit; ///< Message lines the queue holds; markers always fit.
    int queuedMessages = 0;              ///< Message lines in the queue.
    Count queueFullUnmarked = 0;         ///< Dropped from a full queue, not marked yet.
    bool stopRequested = false;
    bool stopping = false; ///< The writer closes the connection: no disconnect marker.
    bool closed = false;   ///< nats.c made the closed callback.

    // ── Replay (natssession.h, REPLAY); set before subscribing ─────────
    /// A full queue makes the message callback wait for room, not drop.
    bool waitForRoom = false;
    std::condition_variable room; ///< Notified when the writer took the queue.
    bool followLive = false;      ///< Go on once caught up.
    /// Skip messages while the consumer has at least this many more
    /// pending, until the first one written: the last N with a filter.
    /// 0: none.  Read and written by nats.c's delivery thread only.
    Count skipWhilePending = 0;
    bool caughtUp = false;   ///< Under the mutex.
    bool replayDone = false; ///< Caught up, not following: the writer ends; under the mutex.

    std::atomic<Count> received{ 0 };    ///< Messages nats.c handed over.
    std::atomic<Count> dropped{ 0 };     ///< Drops counted: marked, or to be marked.
    std::atomic<int> limitMessages{ 0 }; ///< The subscriptions' pending limits, read back.
    std::atomic<int> limitBytes{ 0 };

    /// Whether every subscription with a completion callback has completed;
    /// on the writer, under the mutex.
    bool subscriptionsDoneLocked() const
    {
        return std::all_of( subscriptions.begin(), subscriptions.end(),
                            []( const auto& s ) { return !s->awaitsCompletion || s->done; } );
    }

    /// The state of @p handle; null if it is none of the session's.
    SubscriptionState* find( natsSubscription* handle ) const
    {
        for ( const auto& s : subscriptions ) {
            if ( s->handle.load() == handle ) {
                return s.get();
            }
        }
        return nullptr;
    }

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
            std::unique_lock<std::mutex> lock( mutex );
            if ( waitForRoom ) {
                // Not while stopping: the writer then waits for nats.c, no
                // longer taking the queue.
                room.wait( lock, [ this ] { return queuedMessages < queueLimit || stopping; } );
            }
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
        room.notify_all();
        return batch;
    }

    /// The replay has caught up with its stream: say so, or end it.
    void markCaughtUp()
    {
        {
            std::lock_guard<std::mutex> lock( mutex );
            if ( caughtUp ) {
                return;
            }
            caughtUp = true;
            if ( followLive ) {
                enqueueLocked( formatMarker( nowUs(), "caught up, following live" ), Event::Status,
                               QStringLiteral( "caught up, following live" ) );
            }
            else {
                replayDone = true;
            }
        }
        wake.notify_all();
    }

    /// Queue a dropped= marker for @p count drops for @p reason; under the mutex.
    void pushDroppedLocked( Count count, const char* reason )
    {
        enqueueLocked( formatMarker( nowUs(), "dropped=" + QByteArray::number( count ) + " ("
                                                  + reason + ")" ) );
    }

    /// Mark the slow-consumer drops of @p subscription not marked yet.
    void markSlowConsumer( SubscriptionState& subscription )
    {
        auto* handle = subscription.handle.load();
        int64_t total = 0;
        if ( !handle || natsSubscription_GetDropped( handle, &total ) != NATS_OK ) {
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
            if ( count <= subscription.slowConsumerMarked ) {
                return;
            }
            const auto unmarked = count - subscription.slowConsumerMarked;
            subscription.slowConsumerMarked = count;
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

    /// @p closure is the SubscriptionState of the subscription.
    static void onMessage( natsConnection*, natsSubscription* handle, natsMsg* msg, void* closure )
    {
        auto& subscription = *static_cast<SubscriptionState*>( closure );
        auto* self = subscription.shared;
        // nats.c may deliver before the writer has stored the handle.
        natsSubscription* unset = nullptr;
        subscription.handle.compare_exchange_strong( unset, handle );
        self->received.fetch_add( 1, std::memory_order_relaxed );
        // Drops since the last message: the gap is before this one.
        self->markSlowConsumer( subscription );
        auto line = formatLine( toMessage( msg ), self->format );
        natsMsg_Destroy( msg );
        self->pushMessage( std::move( line ) );
    }

    /// A message of a replay; @p closure is the SubscriptionState of the
    /// subscription.
    static void onReplayMessage( natsConnection*, natsSubscription* handle, natsMsg* msg,
                                 void* closure )
    {
        auto& subscription = *static_cast<SubscriptionState*>( closure );
        auto* self = subscription.shared;
        natsSubscription* unset = nullptr;
        subscription.handle.compare_exchange_strong( unset, handle );

        jsMsgMetaData* meta = nullptr;
        if ( natsMsg_GetMetaData( &meta, msg ) != NATS_OK ) {
            natsMsg_Destroy( msg ); // not a stream's message: nothing to replay
            return;
        }
        const auto pending = static_cast<Count>( meta->NumPending );
        if ( self->skipWhilePending > 0 && pending >= self->skipWhilePending ) {
            jsMsgMetaData_Destroy( meta );
            natsMsg_Destroy( msg ); // before the last N
            return;
        }
        self->skipWhilePending = 0; // from the first written on, all are

        self->received.fetch_add( 1, std::memory_order_relaxed );
        self->markSlowConsumer( subscription );
        auto message = toMessage( msg );
        message.receivedUs = meta->Timestamp / 1000;
        message.streamSequence = meta->Sequence.Stream;
        message.reply.clear(); // nats.c's is the consumer's, the stream keeps none
        jsMsgMetaData_Destroy( meta );
        auto line = formatLine( message, self->format );
        natsMsg_Destroy( msg );
        self->pushMessage( std::move( line ) );
        if ( pending == 0 ) {
            self->markCaughtUp();
        }
    }

    /// @p closure is the SubscriptionState of the subscription.
    static void onSubscriptionComplete( void* closure )
    {
        auto& subscription = *static_cast<SubscriptionState*>( closure );
        auto* self = subscription.shared;
        {
            std::lock_guard<std::mutex> lock( self->mutex );
            subscription.done = true;
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
        for ( const auto& subscription : self->subscriptions ) {
            self->markSlowConsumer( *subscription );
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
            if ( auto* state = self->find( subscription ) ) {
                self->markSlowConsumer( *state );
            }
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
    /// Its secret is cleared once the connection options hold it.
    SessionConfig config;
    const int generation;
    Shared* shared = new Shared; ///< Leaked if nats.c does not let go of it.
    /// Whether shared may be freed: nats.c confirmed its last callback (or
    /// never got it).  Written by the thread, read after it is joined.
    bool released = true;
    /// Opened by start(), before the thread runs; written by the thread.
    RotatingFile file;
    bool writeFailed = false;
    jsCtx* js = nullptr; ///< A replay's JetStream context; the thread's.
    std::thread thread;

    /// The writer of start number @p startGeneration, writing the files
    /// <@p fileStem>.log, <@p fileStem>.2.log, …
    Worker( NatsSession* owner, const SessionConfig& sessionConfig, int startGeneration,
            const QString& fileStem )
        : session( owner )
        , config( sessionConfig )
        , generation( startGeneration )
        , file( fileStem, sessionConfig.maxFileBytes )
    {
        shared->format = config.format;
        shared->queueLimit = config.queueLimit;
        const bool replay = config.replay.enabled();
        shared->waitForRoom = replay;
        shared->followLive = config.replay.followLive;
        const auto count = replay ? 1 : config.subscriptions.size();
        for ( qsizetype i = 0; i < count; ++i ) {
            auto state = std::make_unique<SubscriptionState>();
            state->shared = shared;
            shared->subscriptions.push_back( std::move( state ) );
        }
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

    /// Report @p message as an error, on the GUI thread.
    void postError( const QString& message )
    {
        post( [ message ]( NatsSession* s ) {
            Q_EMIT s->statusChanged( message );
            Q_EMIT s->errorOccurred( message );
        } );
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
            if ( !writeFailed && !file.write( item.line ) ) {
                fail();
            }
            report( item );
        }
        if ( !writeFailed && !batch.empty() && !file.flush() ) {
            fail();
        }
    }

    /// Go on in the next file if a line of @p lineBytes would not fit, and
    /// report what happened.
    void rotateBefore( qint64 lineBytes )
    {
        const auto rotation = file.rotateBefore( lineBytes );
        if ( !rotation.error.isEmpty() ) {
            postError( rotation.error );
        }
        if ( !rotation.path.isEmpty() ) {
            post( [ generation = generation, path = rotation.path ]( NatsSession* s ) {
                s->onWorkerRotated( generation, path );
            } );
        }
    }

    /// The file cannot be written (disk full, volume gone): say so once.
    void fail()
    {
        writeFailed = true;
        postError( file.writeError() );
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
    natsStatus createOptions( natsOptions** options )
    {
        natsStatus status = natsOptions_Create( options );
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

    /// Subscribe to @p wanted on @p connection with the pending limits, as
    /// @p state.
    natsStatus subscribe( natsConnection* connection, const Subscription& wanted,
                          SubscriptionState& state, natsSubscription** subscription )
    {
        const auto subject = wanted.subject.toUtf8();
        const auto queueGroup = wanted.queueGroup.toUtf8();
        natsStatus status
            = queueGroup.isEmpty()
                  ? natsConnection_Subscribe( subscription, connection, subject.constData(),
                                              &Shared::onMessage, &state )
                  : natsConnection_QueueSubscribe( subscription, connection, subject.constData(),
                                                   queueGroup.constData(), &Shared::onMessage,
                                                   &state );
        if ( status == NATS_OK ) {
            status = setUp( *subscription, state );
        }
        return status;
    }

    /// Give @p subscription, made as @p state, its completion callback and
    /// the pending limits.
    natsStatus setUp( natsSubscription* subscription, SubscriptionState& state )
    {
        state.handle = subscription;
        natsStatus status = natsSubscription_SetOnCompleteCB(
            subscription, &Shared::onSubscriptionComplete, &state );
        state.awaitsCompletion = status == NATS_OK;
        if ( status == NATS_OK ) {
            status = natsSubscription_SetPendingLimits( subscription, config.pendingLimits.messages,
                                                        config.pendingLimits.bytes );
        }
        return status;
    }

    /// Subscribe to the stream to replay through an ordered consumer, as
    /// the only SubscriptionState; an error text if that failed.
    QString subscribeReplay( natsConnection* connection, natsSubscription** subscription )
    {
        const auto& replay = config.replay;
        natsStatus status = natsConnection_JetStream( &js, connection, nullptr );
        if ( status != NATS_OK ) {
            return lastError( status );
        }
        const auto stream = replay.stream.toUtf8();
        const auto filter
            = replay.filterSubject.isEmpty() ? QByteArray( ">" ) : replay.filterSubject.toUtf8();

        jsSubOptions options;
        jsSubOptions_Init( &options );
        options.Stream = stream.constData();
        options.Ordered = true; // ephemeral, AckNone, flow control: nats.c's ordered consumer
        options.Config.DeliverPolicy = js_DeliverAll;
        switch ( replay.start ) {
        case ReplayStart::All:
            break;
        case ReplayStart::Last:
            if ( replay.filterSubject.isEmpty() ) {
                jsStreamInfo* info = nullptr;
                jsErrCode error = static_cast<jsErrCode>( 0 );
                status = js_GetStreamInfo( &info, js, stream.constData(), nullptr, &error );
                if ( status != NATS_OK ) {
                    return lastError( status );
                }
                const auto last = info->State.LastSeq;
                const auto first = info->State.FirstSeq;
                jsStreamInfo_Destroy( info );
                if ( last >= replay.lastMessages && last - replay.lastMessages + 1 > first ) {
                    options.Config.OptStartSeq = last - replay.lastMessages + 1;
                }
            }
            else {
                // Which of the stream's messages match is the server's to
                // know: read them all, write the last N.
                shared->skipWhilePending = replay.lastMessages;
            }
            break;
        case ReplayStart::Sequence:
            options.Config.OptStartSeq = replay.sequence;
            break;
        case ReplayStart::Time:
            options.Config.OptStartTime = replay.timeUs * 1000;
            break;
        }

        auto& state = *shared->subscriptions.front();
        jsErrCode error = static_cast<jsErrCode>( 0 );
        status = js_Subscribe( subscription, js, filter.constData(), &Shared::onReplayMessage,
                               &state, nullptr, &options, &error );
        if ( status == NATS_OK ) {
            status = setUp( *subscription, state );
        }
        if ( status != NATS_OK ) {
            return lastError( status );
        }

        // Nothing to replay (an empty stream, a start beyond its end): no
        // message will say that the replay has caught up.
        jsConsumerInfo* info = nullptr;
        status = natsSubscription_GetConsumerInfo( &info, *subscription, nullptr, nullptr );
        if ( status != NATS_OK ) {
            return lastError( status );
        }
        const bool nothing = info->NumPending == 0 && info->Delivered.Consumer == 0;
        jsConsumerInfo_Destroy( info );
        if ( nothing ) {
            shared->markCaughtUp();
        }
        return {};
    }

    /// Connect and subscribe; false, after an error marker, if that failed.
    /// @p subscriptions gets one for each of the config's, null for those
    /// not made.
    bool open( natsOptions** options, natsConnection** connection,
               std::vector<natsSubscription*>& subscriptions )
    {
        const auto fail = [ this ]( const QString& why ) {
            shared->pushMarker( QStringLiteral( "connection to %1 failed: %2" )
                                    .arg( redactUrls( config.url ), why ),
                                Event::Error );
            return false;
        };
        natsStatus status = createOptions( options );
        if ( status != NATS_OK ) {
            return fail( lastError( status ) );
        }
        const auto problem = configureConnection( *options, config );
        // nats.c has its own copy, for reconnecting: no other is kept here.
        config.auth.secret.clear();
        if ( !problem.isEmpty() ) {
            return fail( problem );
        }
        status = natsConnection_Connect( connection, *options );
        if ( status != NATS_OK ) {
            return fail( explainConnectFailure( config, status, lastError( status ) ) );
        }

        const auto subscribeFailed = [ this ]( const QString& what, natsStatus failure ) {
            shared->pushMarker(
                QStringLiteral( "subscribing to %1 failed: %2" ).arg( what, lastError( failure ) ),
                Event::Error );
            return false;
        };
        if ( config.replay.enabled() ) {
            // Before subscribing: the messages come at once, after this line.
            const auto server = connectedUrl( *connection );
            const auto what = describeReplay( config.replay );
            shared->pushMarker(
                QStringLiteral( "connected to %1, replaying %2" ).arg( server, what ),
                Event::Connected, server );
            subscriptions.assign( 1, nullptr );
            const auto error = subscribeReplay( *connection, &subscriptions.front() );
            if ( !error.isEmpty() ) {
                shared->pushMarker( QStringLiteral( "replaying %1 failed: %2" ).arg( what, error ),
                                    Event::Error );
                return false;
            }
            int messages = 0;
            int bytes = 0;
            natsSubscription_GetPendingLimits( subscriptions.front(), &messages, &bytes );
            shared->limitMessages = messages;
            shared->limitBytes = bytes;
            return true;
        }
        subscriptions.assign( config.subscriptions.size(), nullptr );
        for ( int i = 0; i < config.subscriptions.size(); ++i ) {
            const auto& wanted = config.subscriptions.at( i );
            status = subscribe( *connection, wanted, *shared->subscriptions.at( i ),
                                &subscriptions.at( i ) );
            if ( status != NATS_OK ) {
                return subscribeFailed( describeSubscriptions( { wanted } ), status );
            }
        }
        int messages = 0;
        int bytes = 0;
        status = natsSubscription_GetPendingLimits( subscriptions.front(), &messages, &bytes );
        shared->limitMessages = messages;
        shared->limitBytes = bytes;
        if ( status == NATS_OK ) {
            // A round trip: once the server has answered, it has the
            // subscriptions, so the "connected" marker means that every
            // message published from then on is captured.
            status = natsConnection_Flush( *connection );
        }
        const auto subjects = describeSubscriptions( config.subscriptions );
        if ( status != NATS_OK ) {
            return subscribeFailed( subjects, status );
        }

        const auto server = connectedUrl( *connection );
        shared->pushMarker(
            QStringLiteral( "connected to %1, subscribed to %2" ).arg( server, subjects ),
            Event::Connected, server );
        return true;
    }

    /// Why capture() ended.
    enum class End {
        Stopped,   ///< stop()
        Closed,    ///< The connection was closed for good.
        Completed, ///< The replay caught up, not following live.
    };

    /// Write batches until stopped, the connection is closed for good or
    /// the replay is complete.
    End capture()
    {
        for ( ;; ) {
            std::vector<Item> batch;
            bool stop = false;
            bool closed = false;
            bool completed = false;
            {
                std::unique_lock<std::mutex> lock( shared->mutex );
                shared->wake.wait( lock, [ this ] {
                    return !shared->queue.empty() || shared->stopRequested || shared->closed
                           || shared->replayDone;
                } );
                batch = shared->takeLocked();
                stop = shared->stopRequested;
                closed = shared->closed;
                completed = shared->replayDone;
            }
            write( batch );
            if ( stop ) {
                return End::Stopped;
            }
            if ( completed ) {
                return End::Completed;
            }
            if ( closed ) {
                return End::Closed;
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

    /// End the subscriptions without losing what nats.c still holds for
    /// them: nats.c discards the messages pending in a subscription once it
    /// is closed (dispatch.c), uncounted.
    void endSubscriptions( natsConnection* connection,
                           const std::vector<natsSubscription*>& subscriptions )
    {
        // Lifted, nats.c drops nothing more (it decides under the lock that
        // SetPendingLimits takes), so the count read now is the last.
        for ( auto* subscription : subscriptions ) {
            natsSubscription_SetPendingLimits( subscription, -1, -1 );
        }
        for ( const auto& state : shared->subscriptions ) {
            shared->markSlowConsumer( *state );
        }
        // Connected: drain each.  nats.c sends UNSUB, waits for the
        // server's answer (so all that was sent before has arrived),
        // delivers what is pending and then makes the completion callback.
        // With no timeout of its own (-1) it never closes a subscription
        // early.
        bool draining = natsConnection_Status( connection ) == NATS_CONN_STATUS_CONNECTED;
        for ( auto* subscription : subscriptions ) {
            draining = draining && natsSubscription_DrainTimeout( subscription, -1 ) == NATS_OK;
        }
        if ( draining ) {
            writeUntil(
                [ this ] {
                    std::lock_guard<std::mutex> lock( shared->mutex );
                    return shared->subscriptionsDoneLocked();
                },
                kDrainTimeout );
            return;
        }
        // Not connected, nothing more arrives (and a drain would wait for
        // its flush to time out): wait until nats.c has delivered what it
        // holds.  A message it has taken from the queue it delivers even if
        // the subscription is closed meanwhile.
        writeUntil(
            [ &subscriptions ] {
                return std::all_of(
                    subscriptions.begin(), subscriptions.end(), []( natsSubscription* s ) {
                        int pending = 0;
                        return natsSubscription_GetPending( s, &pending, nullptr ) != NATS_OK
                               || pending == 0;
                    } );
            },
            kCallbackTimeout );
        for ( auto* subscription : subscriptions ) {
            natsSubscription_Unsubscribe( subscription ); // a draining one goes on draining
        }
    }

    /// End the subscriptions, close and destroy; whether nats.c confirmed
    /// that it makes no callback any more.
    bool close( natsConnection* connection, std::vector<natsSubscription*> subscriptions )
    {
        {
            std::lock_guard<std::mutex> lock( shared->mutex );
            shared->stopping = true;
        }
        shared->room.notify_all(); // a replay's callback waits no more
        subscriptions.erase( std::remove( subscriptions.begin(), subscriptions.end(), nullptr ),
                             subscriptions.end() );
        if ( !subscriptions.empty() ) {
            endSubscriptions( connection, subscriptions );
        }
        bool confirmed = true;
        if ( connection ) {
            natsConnection_Close( connection );
            confirmed = waitUntil(
                []( const Shared& s ) { return s.closed && s.subscriptionsDoneLocked(); } );
        }
        for ( auto* subscription : subscriptions ) {
            natsSubscription_Destroy( subscription );
        }
        jsCtx_Destroy( js );
        js = nullptr;
        natsConnection_Destroy( connection );
        return confirmed;
    }

    void run()
    {
        natsOptions* options = nullptr;
        natsConnection* connection = nullptr;
        std::vector<natsSubscription*> subscriptions;

        const bool opened = open( &options, &connection, subscriptions );
        auto end = End::Closed;
        if ( opened ) {
            end = capture();
        }
        else {
            std::lock_guard<std::mutex> lock( shared->mutex );
            end = shared->stopRequested ? End::Stopped : End::Closed;
        }

        released = close( connection, subscriptions );
        natsOptions_Destroy( options );

        {
            // Drops no message after them marks yet.
            std::lock_guard<std::mutex> lock( shared->mutex );
            shared->markQueueFullLocked();
        }
        drain(); // what arrived before the subscription ended
        switch ( end ) {
        case End::Stopped:
            shared->pushMarker( QStringLiteral( "stopped" ), Event::Status );
            break;
        case End::Completed:
            shared->pushMarker( QStringLiteral( "replay complete" ), Event::Status );
            break;
        case End::Closed:
            if ( opened ) {
                shared->pushMarker( QStringLiteral( "connection closed" ), Event::Error );
            }
            break;
        }
        drain();
        file.close();

        if ( end == End::Completed ) {
            post( [ generation = generation ]( NatsSession* s ) {
                s->onWorkerCompleted( generation );
            } );
        }
        else if ( end == End::Closed ) {
            post( [ generation = generation ]( NatsSession* s ) {
                s->onWorkerFinished( generation );
            } );
        }
    }
};

// ── NatsSession ─────────────────────────────────────────────────────────

QString describeSubscriptions( const QList<Subscription>& subscriptions )
{
    QStringList names;
    for ( const auto& subscription : subscriptions ) {
        names << ( subscription.queueGroup.isEmpty()
                       ? subscription.subject
                       : QStringLiteral( "%1 (queue %2)" )
                             .arg( subscription.subject, subscription.queueGroup ) );
    }
    return names.join( QStringLiteral( ", " ) );
}

QString describeReplay( const ReplayConfig& replay )
{
    QString text = QStringLiteral( "stream %1" ).arg( replay.stream );
    if ( !replay.filterSubject.isEmpty() ) {
        text += QStringLiteral( ", subject %1" ).arg( replay.filterSubject );
    }
    switch ( replay.start ) {
    case ReplayStart::All:
        break;
    case ReplayStart::Last:
        text += replay.lastMessages == 1
                    ? QStringLiteral( ", the last message" )
                    : QStringLiteral( ", the last %1 messages" ).arg( replay.lastMessages );
        break;
    case ReplayStart::Sequence:
        text += QStringLiteral( ", from sequence %1" ).arg( replay.sequence );
        break;
    case ReplayStart::Time: {
        Message time;
        time.receivedUs = replay.timeUs;
        // The time as the lines have it: the formatter's timestamp.
        const auto line = formatLine( time );
        text += QStringLiteral( ", from %1" ).arg( QString::fromUtf8( line.left( 27 ) ) );
        break;
    }
    }
    if ( replay.followLive ) {
        text += QStringLiteral( ", then live" );
    }
    return text;
}

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
    const auto& replay = config_.replay;
    const auto blank = []( const Subscription& s ) { return s.subject.trimmed().isEmpty(); };
    if ( replay.enabled() ) {
        if ( replay.stream.trimmed().isEmpty() ) {
            return fail( QStringLiteral( "No stream to replay." ) );
        }
        if ( replay.start == ReplayStart::Last && replay.lastMessages == 0 ) {
            return fail( QStringLiteral( "Replaying the last 0 messages replays nothing." ) );
        }
        if ( replay.start == ReplayStart::Sequence && replay.sequence == 0 ) {
            return fail( QStringLiteral( "A stream's sequences start at 1." ) );
        }
    }
    else if ( config_.subscriptions.isEmpty()
              || std::any_of( config_.subscriptions.begin(), config_.subscriptions.end(),
                              blank ) ) {
        return fail( QStringLiteral( "No subject to subscribe to." ) );
    }
    if ( !tempDir_.isValid() ) {
        return fail( QStringLiteral( "Cannot create a temporary directory: %1" )
                         .arg( tempDir_.errorString() ) );
    }

    // Started again, the session goes on in its last file.
    const auto name = replay.enabled()
                          ? "replay_" + safeFileName( replay.stream ).left( kMaxFileNameSubjects )
                          : fileNameFor( config_.subscriptions );
    auto worker = std::make_unique<Worker>( this, config_, ++generation_,
                                            tempDir_.filePath( "nats_" + name ) );
    const bool opened = worker->file.open();
    filePath_ = worker->file.fileName();
    if ( !opened ) {
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

void NatsSession::onWorkerCompleted( int generation )
{
    if ( !worker_ || generation != generation_ ) {
        return; // stopped meanwhile, or the report of an earlier start
    }
    releaseWorker();
    Q_EMIT completed();
}

} // namespace nats_monitor

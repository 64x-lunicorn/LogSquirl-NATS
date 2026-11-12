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
 * @brief Implementation of NatsSession: nats.c's connection, subscriptions
 *        and callbacks, feeding a capture sink (capturesink.h).
 *
 * HOW IT WORKS
 * ────────────
 *   1. start() opens the sink's file, starts the sink's writer thread and
 *      the session's connection thread.
 *   2. The connection thread connects and subscribes to each subject; its
 *      "connected" marker is the capture's first line.  A replay instead
 *      writes the marker and subscribes to the stream through nats.c's
 *      ordered consumer (js_Subscribe, jsSubOptions::Ordered), as the replay
 *      plan made from the stream's info says (replay.h); its callback
 *      takes the stored time and stream sequence from the message's
 *      metadata, and the plan's step says whether to write the message
 *      and whether the replay has caught up (or, before any message, the
 *      plan and the consumer's info).
 *   3. nats.c's callbacks format a line and hand it to the sink, which
 *      queues it for its writer or, full, drops and marks it (a replay's
 *      waits for room).  Each message callback (and the slow-consumer
 *      error, and the disconnected callback) also reads nats.c's dropped
 *      count of its subscription (each, for the disconnected callback) and
 *      has the sink mark what is new.  The sink writes, rotates and
 *      reports; the session forwards its reports to the GUI thread.
 *   4. The connection thread then waits until stop() asks it to end, the
 *      connection is closed for good or the replay is complete.
 *   5. It lifts the pending limits, so nats.c drops nothing more, and marks
 *      the last dropped counts.  It drains each subscription (or, not
 *      connected, waits until nats.c holds nothing for them), the sink
 *      writing all the while, closes the connection, waits until nats.c
 *      has made its last callback, and finishes the sink: what is left, a
 *      "stopped" marker, the file closed.  A replay's sink instead
 *      discards what nats.c still delivers once stopping, uncounted: the
 *      stream keeps it, and writing up to the pending limits of it would
 *      hold the stop up.  Its drain still deletes the consumer.
 *
 * What the callbacks use (Shared, the sink in it) is apart from the
 * worker: should nats.c not confirm within the timeout that its callbacks
 * are over, Shared is leaked rather than freed under a running callback.
 * The sink is finished by then (its writer joined, its file closed), and
 * takes nothing more from a late callback.
 */

// First: on Windows nats.h includes winsock2.h, which must come before any
// windows.h.
#include <nats.h>

#include "natssession.h"

#include "capturesink.h"
#include "natsoptions.h"
#include "profiles.h"
#include "replay.h"
#include "tempdirs.h"

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

using Event = CaptureSink::Event::Kind;

/// nats.c's text for @p status, with @p detail if there is one.
QString describe( natsStatus status, const char* detail )
{
    QString text = QString::fromUtf8( natsStatus_GetText( status ) );
    if ( detail && *detail ) {
        text += QStringLiteral( " (%1)" ).arg( QString::fromUtf8( detail ) );
    }
    return text;
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

/// @p subject with its wildcards spelled out, a token > as "all" and a
/// token * as "any": orders.> is "orders.all", > alone is "all".
QString spelledOut( const QString& subject )
{
    auto tokens = subject.split( QLatin1Char( '.' ) );
    for ( auto& token : tokens ) {
        if ( token == QLatin1String( ">" ) ) {
            token = QStringLiteral( "all" );
        }
        else if ( token == QLatin1String( "*" ) ) {
            token = QStringLiteral( "any" );
        }
    }
    return tokens.join( QLatin1Char( '.' ) );
}

/// The subjects of @p subscriptions as the file's name:
/// "orders.all_billing.any" for orders.> and billing.*, cut to
/// kMaxFileNameSubjects.
QString fileNameFor( const QList<Subscription>& subscriptions )
{
    QStringList subjects;
    for ( const auto& subscription : subscriptions ) {
        subjects << safeFileName( spelledOut( subscription.subject ) );
    }
    return subjects.join( '_' ).left( kMaxFileNameSubjects );
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
    /// connection thread reads and writes it.
    bool awaitsCompletion = false;
    /// A replay's step, made from its plan before subscribing; then used by
    /// nats.c's delivery thread only.
    ReplayStep replayStep;
};

/// What nats.c's callbacks and the connection thread share: the capture
/// sink they feed, and the state of the connection and its subscriptions.
struct Shared {
    /// Writing the files of @p sinkConfig, reporting to @p reporter.
    Shared( const CaptureSink::Config& sinkConfig, CaptureSink::Reporter reporter )
        : sink( sinkConfig, std::move( reporter ) )
    {
    }

    FormatOptions format;

    /// Everything between a message and the file: the callbacks hand it
    /// their lines, from any thread.
    CaptureSink sink;

    /// One for each subscription of the config, made before connecting
    /// and never changed after: the callbacks read it without the mutex.
    std::vector<std::unique_ptr<SubscriptionState>> subscriptions;

    /// Guards the flags below and each SubscriptionState's marked count and
    /// done; taken before the sink's own mutex, never after.
    std::mutex mutex;
    std::condition_variable wake; ///< The flags or a subscription's done changed.
    bool stopRequested = false;
    bool stopping = false; ///< The connection thread closes the connection: no disconnect marker.
    bool closed = false;   ///< nats.c made the closed callback.

    // ── Replay (natssession.h, REPLAY); set before subscribing ─────────
    bool followLive = false; ///< Go on once caught up (ReplayPlan::followLive).
    bool caughtUp = false;   ///< Under the mutex.
    bool replayDone = false; ///< Caught up, not following: the capture ends; under the mutex.

    std::atomic<int> limitMessages{ 0 }; ///< The subscriptions' pending limits, read back.
    std::atomic<int> limitBytes{ 0 };

    /// Whether every subscription with a completion callback has completed;
    /// on the connection thread, under the mutex.
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
        return { sink.received(), sink.dropped(), { limitMessages.load(), limitBytes.load() } };
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
                // Under the mutex: before what a callback queues next.
                sink.marker( QStringLiteral( "caught up, following live" ), Event::Status );
            }
            else {
                replayDone = true;
            }
        }
        wake.notify_all();
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
        std::lock_guard<std::mutex> lock( mutex );
        if ( count <= subscription.slowConsumerMarked ) {
            return;
        }
        // Under the mutex: two callbacks that read the count mark each drop once.
        sink.dropped( count - subscription.slowConsumerMarked, "slow consumer" );
        subscription.slowConsumerMarked = count;
    }

    /// The SubscriptionState @p closure of a message callback for @p handle.
    static SubscriptionState& subscriptionOf( void* closure, natsSubscription* handle )
    {
        auto& subscription = *static_cast<SubscriptionState*>( closure );
        // nats.c may deliver before the connection thread has stored the handle.
        natsSubscription* unset = nullptr;
        subscription.handle.compare_exchange_strong( unset, handle );
        return subscription;
    }

    /// Mark the drops before @p msg, a message of @p subscription, and hand
    /// its line, the Message @p toLine makes of it, to the sink.  Destroys
    /// @p msg.
    template <typename ToLine>
    void take( SubscriptionState& subscription, natsMsg* msg, ToLine toLine )
    {
        // Drops since the last message: the gap is before this one.
        markSlowConsumer( subscription );
        auto line = formatLine( toLine(), format );
        natsMsg_Destroy( msg );
        sink.message( std::move( line ) );
    }

    // ── nats.c callbacks, on nats.c's threads ────────────────────────

    /// @p closure is the SubscriptionState of the subscription.
    static void onMessage( natsConnection*, natsSubscription* handle, natsMsg* msg, void* closure )
    {
        auto& subscription = subscriptionOf( closure, handle );
        subscription.shared->take( subscription, msg, [ msg ] { return toMessage( msg ); } );
    }

    /// A message of a replay; @p closure is the SubscriptionState of the
    /// subscription.
    static void onReplayMessage( natsConnection*, natsSubscription* handle, natsMsg* msg,
                                 void* closure )
    {
        auto& subscription = subscriptionOf( closure, handle );
        auto* self = subscription.shared;
        if ( self->sink.discarding() ) {
            // Stopped: what nats.c still holds, up to the pending limits,
            // the stream keeps; writing it would hold up the stop.
            natsMsg_Destroy( msg );
            return;
        }

        jsMsgMetaData* meta = nullptr;
        if ( natsMsg_GetMetaData( &meta, msg ) != NATS_OK ) {
            natsMsg_Destroy( msg ); // not a stream's message: nothing to replay
            return;
        }
        const auto step = subscription.replayStep.next( static_cast<quint64>( meta->NumPending ),
                                                        meta->Timestamp );
        const auto storedUs = meta->Timestamp / 1000;
        const auto sequence = meta->Sequence.Stream;
        jsMsgMetaData_Destroy( meta );
        if ( step.write ) {
            self->take( subscription, msg, [ msg, storedUs, sequence ] {
                auto message = toMessage( msg );
                message.receivedUs = storedUs;
                message.streamSequence = sequence;
                message.reply.clear(); // nats.c's is the consumer's, the stream keeps none
                return message;
            } );
        }
        else {
            natsMsg_Destroy( msg ); // before where the replay starts
        }
        if ( step.caughtUp ) {
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
        // connection now be closed for good.  The connection thread
        // destroys the subscription only after the closed callback, which
        // comes after this one.
        for ( const auto& subscription : self->subscriptions ) {
            self->markSlowConsumer( *subscription );
        }
        self->sink.marker( QStringLiteral( "disconnected" ), Event::Disconnected );
    }

    static void onReconnected( natsConnection* connection, void* closure )
    {
        const auto server = connectedUrl( connection );
        static_cast<Shared*>( closure )->sink.marker(
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
        self->sink.marker( QStringLiteral( "error: %1" ).arg( describe( status, detail ) ),
                           Event::Error );
    }
};

} // namespace

// ── The connection thread ───────────────────────────────────────────────

struct NatsSession::Worker {
    NatsSession* const session; ///< Only posted to; it joins the thread before it goes.
    /// Its secret is cleared once the connection options hold it.
    SessionConfig config;
    const int generation;
    /// Leaked if nats.c does not let go of it; its sink is opened by
    /// start(), before the threads run.
    Shared* shared;
    /// Whether shared may be freed: nats.c confirmed its last callback (or
    /// never got it).  Written by the thread, read after it is joined.
    bool released = true;
    jsCtx* js = nullptr; ///< A replay's JetStream context; the thread's.
    std::thread thread;

    /// The worker of start number @p startGeneration, writing the files
    /// <@p fileStem>.log, <@p fileStem>.2.log, …
    Worker( NatsSession* owner, const SessionConfig& sessionConfig, int startGeneration,
            const QString& fileStem )
        : session( owner )
        , config( sessionConfig )
        , generation( startGeneration )
        , shared( new Shared( sinkConfig( sessionConfig, fileStem ),
                              [ owner, startGeneration ]( const CaptureSink::Event& event ) {
                                  forward( owner, startGeneration, event );
                              } ) )
    {
        shared->format = config.format;
        const auto count = config.replay.enabled() ? 1 : config.subscriptions.size();
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
            delete shared; // finishes the sink, if the thread never ran
        }
    }

    Worker( const Worker& ) = delete;
    Worker& operator=( const Worker& ) = delete;

    /// The sink of @p config: a live capture drops when its queue is full,
    /// a replay waits for room.
    static CaptureSink::Config sinkConfig( const SessionConfig& config, const QString& fileStem )
    {
        CaptureSink::Config sink;
        sink.fileStem = fileStem;
        sink.maxFileBytes = config.maxFileBytes;
        sink.queueLimit = config.queueLimit;
        sink.mode = config.replay.enabled() ? CaptureSink::Mode::WaitForRoom
                                            : CaptureSink::Mode::Drop;
        return sink;
    }

    /// Emit a signal of @p session on the GUI thread.  Safe from the
    /// sink's writer thread and the connection thread: the session joins
    /// them before it is destroyed, and Qt drops a call still queued then.
    template <typename Emitter>
    static void post( NatsSession* session, Emitter emitter )
    {
        QMetaObject::invokeMethod(
            session, [ session, emitter ] { emitter( session ); }, Qt::QueuedConnection );
    }

    template <typename Emitter>
    void post( Emitter emitter )
    {
        post( session, emitter );
    }

    /// Have @p session emit @p transition, of start number @p generation,
    /// on the GUI thread: unless it was stopped meanwhile.
    static void postTransition( NatsSession* session, int generation,
                                const Transition& transition )
    {
        post( session, [ generation, transition ]( NatsSession* s ) {
            s->onWorkerTransition( generation, transition );
        } );
    }

    /// Forward @p event, which the sink of start number @p generation
    /// reports on its writer thread, to @p session's transitions and
    /// signals: one each, by its kind.
    static void forward( NatsSession* session, int generation, const CaptureSink::Event& event )
    {
        using Kind = Transition::Kind;
        switch ( event.kind ) {
        case Event::Connected:
            postTransition( session, generation,
                            { Kind::Running, QStringLiteral( "connected to %1" ).arg( event.text ),
                              event.text } );
            return;
        case Event::Reconnected:
            postTransition( session, generation,
                            { Kind::Running,
                              QStringLiteral( "reconnected to %1" ).arg( event.text ),
                              event.text } );
            return;
        case Event::Disconnected:
            postTransition( session, generation, { Kind::Reconnecting, event.text, {} } );
            return;
        case Event::Status:
            return; // in the file; the transitions say what it changes
        case Event::Error:
        case Event::FileError:
            post( session,
                  [ text = event.text ]( NatsSession* s ) { Q_EMIT s->errorOccurred( text ); } );
            return;
        case Event::Rotated:
            post( session, [ generation, path = event.text ]( NatsSession* s ) {
                s->onWorkerRotated( generation, path );
            } );
            return;
        }
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

    /// The state of @p stream to plan its replay from; an error text if it
    /// could not be read.
    QString readStream( const QByteArray& stream, StreamState* state )
    {
        jsStreamInfo* info = nullptr;
        const natsStatus status = js_GetStreamInfo( &info, js, stream.constData(), nullptr,
                                                    nullptr );
        if ( status != NATS_OK ) {
            return describeStatus( status );
        }
        switch ( info->Config->Retention ) {
        case js_LimitsPolicy:
            state->retention = Retention::Limits;
            break;
        case js_InterestPolicy:
            state->retention = Retention::Interest;
            break;
        case js_WorkQueuePolicy:
            state->retention = Retention::WorkQueue;
            break;
        }
        state->firstSequence = info->State.FirstSeq;
        state->lastSequence = info->State.LastSeq;
        state->messages = info->State.Msgs;
        jsStreamInfo_Destroy( info );
        return {};
    }

    /// Subscribe to the stream to replay through an ordered consumer, as
    /// the only SubscriptionState, as its plan (replay.h) says; an error
    /// text if that failed or the plan refused it.
    QString subscribeReplay( natsConnection* connection, natsSubscription** subscription )
    {
        natsStatus status = natsConnection_JetStream( &js, connection, nullptr );
        if ( status != NATS_OK ) {
            return describeStatus( status );
        }
        const auto stream = config.replay.stream.toUtf8();
        StreamState streamState;
        if ( const auto error = readStream( stream, &streamState ); !error.isEmpty() ) {
            return error;
        }
        const auto plan = planReplay( config.replay, streamState );
        if ( !plan.refusal.isEmpty() ) {
            return plan.refusal;
        }
        if ( !plan.warning.isEmpty() ) {
            shared->sink.marker( plan.warning, Event::Status );
        }

        jsSubOptions options;
        jsSubOptions_Init( &options );
        options.Stream = stream.constData();
        options.Ordered = true; // ephemeral, AckNone, flow control: nats.c's ordered consumer
        // Stop deletes the consumer; this covers a crash or a connection
        // lost for good.  nats.c keeps it when it recreates the consumer.
        options.Config.InactiveThreshold = plan.inactiveThresholdNs;
        switch ( plan.deliver ) {
        case DeliverFrom::All:
            options.Config.DeliverPolicy = js_DeliverAll;
            break;
        case DeliverFrom::StartSequence:
            options.Config.DeliverPolicy = js_DeliverByStartSequence;
            options.Config.OptStartSeq = plan.startSequence;
            break;
        case DeliverFrom::StartTime:
            options.Config.DeliverPolicy = js_DeliverByStartTime;
            options.Config.OptStartTime = plan.startTimeNs;
            break;
        }

        // Before subscribing: nats.c's delivery thread uses them from then on.
        auto& state = *shared->subscriptions.front();
        state.replayStep = ReplayStep( plan.skip );
        shared->followLive = plan.followLive;
        jsErrCode error = static_cast<jsErrCode>( 0 );
        status = js_Subscribe( subscription, js, plan.filter.constData(), &Shared::onReplayMessage,
                               &state, nullptr, &options, &error );
        if ( status == NATS_OK ) {
            status = setUp( *subscription, state );
        }
        if ( status != NATS_OK ) {
            return describeStatus( status );
        }

        // Nothing to replay (an empty stream, a start beyond its end): no
        // message will say that the replay has caught up.
        jsConsumerInfo* info = nullptr;
        status = natsSubscription_GetConsumerInfo( &info, *subscription, nullptr, nullptr );
        if ( status != NATS_OK ) {
            return describeStatus( status );
        }
        const bool caughtUp
            = caughtUpOnSubscribing( plan, info->NumPending, info->Delivered.Consumer );
        jsConsumerInfo_Destroy( info );
        if ( caughtUp ) {
            shared->markCaughtUp();
        }
        return {};
    }

    /// Write the marker of why the session failed, @p why, and return it.
    /// Reported as the Failed transition once the capture has ended.
    QString failed( const QString& why )
    {
        shared->sink.marker( why, Event::Status );
        return why;
    }

    /// Connect and subscribe; why that failed, after its marker, or empty.
    /// @p subscriptions gets one for each of the config's, null for those
    /// not made.
    QString open( natsOptions** options, natsConnection** connection,
                  std::vector<natsSubscription*>& subscriptions )
    {
        const auto fail = [ this ]( const QString& why ) {
            return failed( QStringLiteral( "connection to %1 failed: %2" )
                               .arg( redactUrls( config.url ), why ) );
        };
        natsStatus status = createOptions( options );
        if ( status != NATS_OK ) {
            return fail( describeStatus( status ) );
        }
        const auto problem = connectTo( *options, config, connection );
        // nats.c has its own copy, for reconnecting: no other is kept here.
        config.auth.secret.clear();
        if ( !problem.isEmpty() ) {
            return failed( problem );
        }

        const auto subscribeFailed = [ this ]( const QString& what, natsStatus failure ) {
            return failed( QStringLiteral( "subscribing to %1 failed: %2" )
                               .arg( what, describeStatus( failure ) ) );
        };
        if ( config.replay.enabled() ) {
            // Before subscribing: the messages come at once, after this line.
            const auto server = connectedUrl( *connection );
            const auto what = describeReplay( config.replay );
            shared->sink.marker(
                QStringLiteral( "connected to %1, replaying %2" ).arg( server, what ),
                Event::Connected, server );
            subscriptions.assign( 1, nullptr );
            const auto error = subscribeReplay( *connection, &subscriptions.front() );
            if ( !error.isEmpty() ) {
                return failed( QStringLiteral( "replaying %1 failed: %2" ).arg( what, error ) );
            }
            int messages = 0;
            int bytes = 0;
            status = natsSubscription_GetPendingLimits( subscriptions.front(), &messages, &bytes );
            if ( status != NATS_OK ) {
                return failed( QStringLiteral( "replaying %1 failed: %2" )
                                   .arg( what, describeStatus( status ) ) );
            }
            shared->limitMessages = messages;
            shared->limitBytes = bytes;
            return {};
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
        shared->sink.marker(
            QStringLiteral( "connected to %1, subscribed to %2" ).arg( server, subjects ),
            Event::Connected, server );
        return {};
    }

    /// Why the capture ended.
    enum class End {
        Stopped,   ///< stop()
        Closed,    ///< The connection was closed for good.
        Completed, ///< The replay caught up, not following live.
    };

    /// Wait until stopped, the connection is closed for good or the replay
    /// is complete; the sink writes meanwhile.
    End awaitEnd()
    {
        std::unique_lock<std::mutex> lock( shared->mutex );
        shared->wake.wait( lock, [ this ] {
            return shared->stopRequested || shared->closed || shared->replayDone;
        } );
        if ( shared->stopRequested ) {
            return End::Stopped;
        }
        return shared->replayDone ? End::Completed : End::Closed;
    }

    /// Poll @p done (called without the mutex) until it holds, or @p timeout
    /// passed, the sink writing what the callbacks hand it meanwhile;
    /// whether it holds.
    template <typename Done>
    static bool pollUntil( Done done, std::chrono::milliseconds timeout )
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while ( !done() ) {
            if ( std::chrono::steady_clock::now() >= deadline ) {
                return false;
            }
            std::this_thread::sleep_for( std::chrono::milliseconds( 10 ) );
        }
        return true;
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
            // The sink writes what the drain delivers meanwhile.
            std::unique_lock<std::mutex> lock( shared->mutex );
            shared->wake.wait_for( lock, kDrainTimeout,
                                   [ this ] { return shared->subscriptionsDoneLocked(); } );
            return;
        }
        // Not connected, nothing more arrives (and a drain would wait for
        // its flush to time out): wait until nats.c has delivered what it
        // holds.  A message it has taken from the queue it delivers even if
        // the subscription is closed meanwhile.
        pollUntil(
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
        if ( config.replay.enabled() ) {
            // A replay writes nothing more, and its callback waits no more.
            shared->sink.discardMessages();
        }
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

        const auto failure = open( &options, &connection, subscriptions );
        const bool opened = failure.isEmpty();
        auto end = End::Closed;
        if ( opened ) {
            end = awaitEnd();
        }
        else {
            std::lock_guard<std::mutex> lock( shared->mutex );
            end = shared->stopRequested ? End::Stopped : End::Closed;
        }

        released = close( connection, subscriptions );
        natsOptions_Destroy( options );

        // What arrived before the subscriptions ended, the drops not marked
        // yet and the last marker; the sink's writer is joined then, so
        // every report it posted comes before the transition below.
        auto& sink = shared->sink;
        using Kind = Transition::Kind;
        Transition ended;
        switch ( end ) {
        case End::Stopped:
            // stop() reports the transition, once it has joined this thread.
            sink.finish( QStringLiteral( "stopped" ), Event::Status );
            return;
        case End::Completed:
            ended = { Kind::Completed, QStringLiteral( "replay complete" ), {} };
            break;
        case End::Closed:
            ended = { Kind::Failed, opened ? QStringLiteral( "connection closed" ) : failure, {} };
            break;
        }
        // Its marker is written already if it never opened.
        sink.finish( opened ? ended.detail : QString(), Event::Status );
        post( [ generation = generation, ended ]( NatsSession* s ) {
            s->onWorkerEnded( generation, ended );
        } );
    }
};

// ── NatsSession ─────────────────────────────────────────────────────────

NatsSession::NatsSession( const SessionConfig& config, const QString& tempRoot, QObject* parent )
    : QObject( parent )
    , config_( config )
    , tempDir_( tempDirTemplate( tempRoot ) )
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
        Q_EMIT transitioned( { Transition::Kind::Failed, message, {} } );
        return false;
    };
    // A backstop: Sessions makes the same check first.
    if ( const auto problem = startProblem( config_ ); !problem.isEmpty() ) {
        return fail( problem );
    }
    const auto& replay = config_.replay;
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
    auto& sink = worker->shared->sink;
    const bool opened = sink.open();
    filePath_ = sink.fileName();
    if ( !opened ) {
        return fail( QStringLiteral( "Cannot create %1: %2" ).arg( filePath_, sink.errorString() ) );
    }
    sink.start();
    auto* raw = worker.get();
    worker->thread = std::thread( [ raw ] { raw->run(); } );
    worker_ = std::move( worker );
    Q_EMIT transitioned( { Transition::Kind::Connecting,
                           QStringLiteral( "connecting to %1…" ).arg( redactUrls( config_.url ) ),
                           {} } );
    return true;
}

void NatsSession::stop()
{
    if ( !worker_ ) {
        return;
    }
    releaseWorker();
    Q_EMIT transitioned( { Transition::Kind::Stopped, QStringLiteral( "stopped" ), {} } );
}

void NatsSession::releaseWorker()
{
    if ( !worker_ ) {
        return;
    }
    worker_->join(); // asks the connection thread to stop, and waits for it and the writer
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

void NatsSession::onWorkerTransition( int generation, const Transition& transition )
{
    if ( !worker_ || generation != generation_ ) {
        return; // stopped meanwhile: Stopped was the last
    }
    Q_EMIT transitioned( transition );
}

void NatsSession::onWorkerEnded( int generation, const Transition& end )
{
    if ( !worker_ || generation != generation_ ) {
        return; // stopped meanwhile, or the report of an earlier start
    }
    releaseWorker(); // the thread has ended: joins at once
    Q_EMIT transitioned( end );
}

} // namespace nats_monitor

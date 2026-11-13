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
 * @file replaysubscription.cpp
 * @brief Implementation of replaysubscription.h: the stream's info, the
 *        ordered consumer, a replayed message and catching up.
 */

// First: on Windows nats.h includes winsock2.h, which must come before any
// windows.h.
#include <nats.h>

#include "replaysubscription.h"

#include "natsmessage.h"
#include "natsoptions.h"

namespace nats_monitor {

ReplaySubscription::ReplaySubscription( const ReplayConfig& replay, Outlet& outlet )
    : replay_( replay )
    , outlet_( outlet )
{
}

ReplaySubscription::~ReplaySubscription()
{
    jsCtx_Destroy( js_ );
}

QString ReplaySubscription::readStream( StreamState* state )
{
    const auto stream = replay_.stream.toUtf8();
    jsStreamInfo* info = nullptr;
    const natsStatus status = js_GetStreamInfo( &info, js_, stream.constData(), nullptr, nullptr );
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

QString ReplaySubscription::subscribe( natsConnection* connection, natsSubscription** subscription )
{
    natsStatus status = natsConnection_JetStream( &js_, connection, nullptr );
    if ( status != NATS_OK ) {
        return describeStatus( status );
    }
    StreamState streamState;
    if ( const auto error = readStream( &streamState ); !error.isEmpty() ) {
        return error;
    }
    const auto plan = planReplay( replay_, streamState );
    if ( !plan.refusal.isEmpty() ) {
        return plan.refusal;
    }
    if ( !plan.warning.isEmpty() ) {
        outlet_.marker( plan.warning );
    }

    const auto stream = replay_.stream.toUtf8();
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
    step_ = ReplayStep( plan.skip );
    followLive_ = plan.followLive;
    jsErrCode error = static_cast<jsErrCode>( 0 );
    status = js_Subscribe( subscription, js_, plan.filter.constData(), &onMessage, this, nullptr,
                           &options, &error );
    if ( status == NATS_OK ) {
        status = outlet_.subscribed( *subscription );
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
    const bool caughtUp = caughtUpOnSubscribing( plan, info->NumPending, info->Delivered.Consumer );
    jsConsumerInfo_Destroy( info );
    if ( caughtUp ) {
        markCaughtUp();
    }
    return {};
}

void ReplaySubscription::markCaughtUp()
{
    {
        std::lock_guard<std::mutex> lock( mutex_ );
        if ( caughtUp_ ) {
            return;
        }
        caughtUp_ = true;
        if ( followLive_ ) {
            // Under the mutex: before what a callback queues next.
            outlet_.marker( QStringLiteral( "caught up, following live" ) );
            return;
        }
    }
    outlet_.completed();
}

void ReplaySubscription::onMessage( natsConnection*, natsSubscription* handle, natsMsg* msg,
                                    void* closure )
{
    auto& self = *static_cast<ReplaySubscription*>( closure );
    if ( self.outlet_.discarding() ) {
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
    const auto step = self.step_.next( static_cast<quint64>( meta->NumPending ), meta->Timestamp );
    const auto storedUs = meta->Timestamp / 1000;
    const auto sequence = meta->Sequence.Stream;
    jsMsgMetaData_Destroy( meta );
    if ( step.write ) {
        auto message = toMessage( msg );
        message.receivedUs = storedUs;
        message.streamSequence = sequence;
        message.reply.clear(); // nats.c's is the consumer's, the stream keeps none
        self.outlet_.message( handle, message );
    }
    natsMsg_Destroy( msg ); // written, or before where the replay starts
    if ( step.caughtUp ) {
        self.markCaughtUp();
    }
}

} // namespace nats_monitor

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
 * @file replay.cpp
 * @brief Implementation of replay.h: describing, checking and choosing a replay.
 */

// First: on Windows nats.h includes winsock2.h, which must come before any
// windows.h.
#include <nats.h>

#include "replay.h"

#include "lineformat.h"
#include "natsoptions.h"

namespace nats_monitor {

namespace {

/// How long listStreams() waits for each server to connect, and for each
/// answer: the dialog waits for it when it closes (at shutdown).
constexpr int kListTimeoutMs = 2000;

} // namespace

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
    case ReplayStart::Time:
        // The time as the lines have it.
        text += QStringLiteral( ", from %1" )
                    .arg( QString::fromLatin1( formatTimestamp( replay.timeUs ) ) );
        break;
    }
    if ( replay.followLive ) {
        text += QStringLiteral( ", then live" );
    }
    return text;
}

QString replayProblem( const ReplayConfig& replay )
{
    if ( replay.stream.trimmed().isEmpty() ) {
        return QStringLiteral( "No stream to replay." );
    }
    if ( replay.start == ReplayStart::Last && replay.lastMessages == 0 ) {
        return QStringLiteral( "Replaying the last 0 messages replays nothing." );
    }
    if ( replay.start == ReplayStart::Sequence && replay.sequence == 0 ) {
        return QStringLiteral( "A stream's sequences start at 1." );
    }
    return {};
}

QStringList listStreams( const SessionConfig& config, QString* error )
{
    natsOptions* options = nullptr;
    natsConnection* connection = nullptr;
    jsCtx* js = nullptr;
    jsStreamNamesList* list = nullptr;
    QStringList names;
    QString problem;

    natsStatus status = natsOptions_Create( &options );
    if ( status == NATS_OK ) {
        status = natsOptions_SetTimeout( options, kListTimeoutMs );
    }
    if ( status != NATS_OK ) {
        problem = describeStatus( status );
    }
    if ( problem.isEmpty() ) {
        problem = connectTo( options, config, &connection );
    }
    if ( problem.isEmpty() ) {
        jsOptions jsOptions;
        jsOptions_Init( &jsOptions );
        jsOptions.Wait = kListTimeoutMs;
        status = natsConnection_JetStream( &js, connection, &jsOptions );
        jsErrCode code = static_cast<jsErrCode>( 0 );
        if ( status == NATS_OK ) {
            status = js_StreamNames( &list, js, nullptr, &code );
        }
        if ( status == NATS_NOT_FOUND ) {
            status = NATS_OK; // no stream
        }
        if ( status != NATS_OK ) {
            problem = QStringLiteral( "listing the streams failed: %1" )
                          .arg( describeStatus( status ) );
        }
    }
    if ( list ) {
        for ( int i = 0; i < list->Count; ++i ) {
            names << QString::fromUtf8( list->List[ i ] );
        }
        jsStreamNamesList_Destroy( list );
    }
    jsCtx_Destroy( js );
    natsConnection_Destroy( connection );
    natsOptions_Destroy( options );

    names.sort();
    if ( error ) {
        *error = problem;
    }
    return problem.isEmpty() ? names : QStringList{};
}

} // namespace nats_monitor

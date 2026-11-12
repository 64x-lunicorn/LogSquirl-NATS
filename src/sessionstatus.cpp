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
 * @file sessionstatus.cpp
 * @brief Implementation of the status of a connection.
 */

#include "sessionstatus.h"

namespace nats_monitor {

void ConnectionStatus::apply( const Transition& transition )
{
    using Kind = Transition::Kind;
    switch ( transition.kind ) {
    case Kind::Connecting:
        status_ = {};
        sampled_ = false;
        status_.state = SessionState::Connecting;
        break;
    case Kind::Running:
        status_.state = SessionState::Running;
        status_.server = transition.server;
        break;
    case Kind::Reconnecting:
        status_.state = SessionState::Connecting;
        status_.server.clear();
        break;
    case Kind::Failed:
    case Kind::Completed:
    case Kind::Stopped:
        status_.state
            = transition.kind == Kind::Failed ? SessionState::Failed : SessionState::Stopped;
        status_.server.clear();
        status_.rate = 0;
        break;
    }
    status_.detail = transition.detail;
}

void ConnectionStatus::opened( const QString& path )
{
    status_.filePath = path;
    status_.files = 1;
}

void ConnectionStatus::rotated( const QString& path )
{
    status_.filePath = path;
    ++status_.files;
}

void ConnectionStatus::errorReported( const QString& text )
{
    status_.detail = text;
}

void ConnectionStatus::sample( quint64 received, quint64 dropped, qint64 nowMs )
{
    status_.received = received;
    status_.dropped = dropped;
    if ( !sampled_ ) {
        sampled_ = true;
    }
    else if ( nowMs > sampledAtMs_ ) {
        const auto since = received >= sampledReceived_ ? received - sampledReceived_ : 0;
        status_.rate = since * 1000 / static_cast<quint64>( nowMs - sampledAtMs_ );
    }
    else {
        return; // no time has passed: the rate counts from the earlier sample
    }
    sampledReceived_ = received;
    sampledAtMs_ = nowMs;
}

} // namespace nats_monitor

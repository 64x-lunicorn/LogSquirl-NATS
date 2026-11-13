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
 * @file natsmessage.cpp
 * @brief Implementation of natsmessage.h.
 */

#include "natsmessage.h"

#include <algorithm>
#include <cstdlib>
#include <vector>

namespace nats_monitor {

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

} // namespace nats_monitor

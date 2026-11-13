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
 * @file secrets.cpp
 * @brief Implementation of wiping a secret.
 */

#include "secrets.h"

#include <openssl/crypto.h>

namespace nats_monitor {

void wipe( QByteArray& bytes )
{
    // data() of a shared array would copy it first: a copy only to wipe.
    if ( bytes.isDetached() && !bytes.isEmpty() ) {
        OPENSSL_cleanse( bytes.data(), static_cast<size_t>( bytes.size() ) );
    }
    bytes.clear();
}

void wipe( QString& text )
{
    if ( text.isDetached() && !text.isEmpty() ) {
        OPENSSL_cleanse( text.data(), static_cast<size_t>( text.size() ) * sizeof( QChar ) );
    }
    text.clear();
}

} // namespace nats_monitor

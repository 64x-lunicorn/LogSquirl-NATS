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
 * @file nkey.cpp
 * @brief Implementation of the NKey public key derivation.
 */

#include "nkey.h"

#include <QRegularExpression>
#include <QString>

#include <openssl/evp.h>

#include <cstring>
#include <memory>

namespace nats_monitor {

namespace {

// ── NKeys ────────────────────────────────────────────────────────────────
// An NKey is base32 (RFC 4648, no padding) of: a prefix, the 32 bytes of
// an ed25519 key, and a CRC-16/XMODEM of both, little-endian.  A user's
// seed starts with "SU" (two prefix bytes), its public key with "U".

constexpr char kBase32[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
constexpr unsigned char kPrefixSeed = 18 << 3;
constexpr unsigned char kPrefixUser = 20 << 3;

quint16 crc16( const QByteArray& data )
{
    quint16 crc = 0;
    for ( const char c : data ) {
        crc ^= static_cast<quint16>( static_cast<unsigned char>( c ) << 8 );
        for ( int i = 0; i < 8; ++i ) {
            crc = ( crc & 0x8000 ) ? static_cast<quint16>( ( crc << 1 ) ^ 0x1021 )
                                   : static_cast<quint16>( crc << 1 );
        }
    }
    return crc;
}

QByteArray base32Decode( const QByteArray& text )
{
    QByteArray out;
    quint32 buffer = 0;
    int bits = 0;
    for ( const char c : text ) {
        const char* at = std::strchr( kBase32, c );
        if ( !at || !c ) {
            return {};
        }
        buffer = ( buffer << 5 ) | static_cast<quint32>( at - kBase32 );
        bits += 5;
        if ( bits >= 8 ) {
            bits -= 8;
            out.append( static_cast<char>( ( buffer >> bits ) & 0xff ) );
        }
    }
    return out;
}

QByteArray base32Encode( const QByteArray& data )
{
    QByteArray out;
    quint32 buffer = 0;
    int bits = 0;
    for ( const char c : data ) {
        buffer = ( buffer << 8 ) | static_cast<unsigned char>( c );
        bits += 8;
        while ( bits >= 5 ) {
            bits -= 5;
            out.append( kBase32[ ( buffer >> bits ) & 31 ] );
        }
    }
    if ( bits > 0 ) {
        out.append( kBase32[ ( buffer << ( 5 - bits ) ) & 31 ] );
    }
    return out;
}

/// The public key ("U…") of the user seed @p seed ("SU…"); empty if it is none.
QByteArray publicKeyOfSeed( const QByteArray& seed )
{
    const auto raw = base32Decode( seed );
    if ( raw.size() != 2 + 32 + 2 ) {
        return {};
    }
    const auto body = raw.left( 34 );
    const auto crc = static_cast<quint16>( static_cast<unsigned char>( raw.at( 34 ) )
                                           | static_cast<unsigned char>( raw.at( 35 ) ) << 8 );
    const auto b1 = static_cast<unsigned char>( raw.at( 0 ) );
    const auto b2 = static_cast<unsigned char>( raw.at( 1 ) );
    const auto type = static_cast<unsigned char>( ( b1 & 7 ) << 5 | ( b2 & 0xf8 ) >> 3 );
    if ( crc16( body ) != crc || ( b1 & 0xf8 ) != kPrefixSeed || type != kPrefixUser ) {
        return {};
    }

    const std::unique_ptr<EVP_PKEY, decltype( &EVP_PKEY_free )> key(
        EVP_PKEY_new_raw_private_key(
            EVP_PKEY_ED25519, nullptr,
            reinterpret_cast<const unsigned char*>( body.constData() + 2 ), 32 ),
        &EVP_PKEY_free );
    unsigned char publicKey[ 32 ] = {};
    size_t length = sizeof publicKey;
    if ( !key || EVP_PKEY_get_raw_public_key( key.get(), publicKey, &length ) != 1
         || length != sizeof publicKey ) {
        return {};
    }
    QByteArray encoded( 1, static_cast<char>( kPrefixUser ) );
    encoded.append( reinterpret_cast<const char*>( publicKey ), sizeof publicKey );
    const auto sum = crc16( encoded );
    encoded.append( static_cast<char>( sum & 0xff ) );
    encoded.append( static_cast<char>( sum >> 8 ) );
    return base32Encode( encoded );
}

} // namespace

QByteArray nkeyUserPublicKey( const QByteArray& seedFile )
{
    // As nats.c reads it: the first user seed in the file.
    static const QRegularExpression seed( QStringLiteral( "SU[A-Z2-7]{56}" ) );
    const auto match = seed.match( QString::fromLatin1( seedFile ) );
    return match.hasMatch() ? publicKeyOfSeed( match.captured().toLatin1() ) : QByteArray();
}

} // namespace nats_monitor

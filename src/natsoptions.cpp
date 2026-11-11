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
 * @file natsoptions.cpp
 * @brief Implementation of the connection options and failure explanations.
 */

#include "natsoptions.h"

#include <QFile>
#include <QRegularExpression>
#include <QUrl>

#include <openssl/evp.h>

#include <cstring>
#include <memory>
#include <utility>
#include <vector>

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
QByteArray userPublicKey( const QByteArray& seed )
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

// ── Files ────────────────────────────────────────────────────────────────

/// Empty if @p path can be read, else why not, naming it as @p what.
QString checkReadable( const QString& path, const QString& what )
{
    QFile file( path );
    if ( path.isEmpty() ) {
        return QStringLiteral( "no %1 given" ).arg( what );
    }
    if ( !file.open( QIODevice::ReadOnly ) ) {
        return QStringLiteral( "cannot read the %1 %2: %3" ).arg( what, path, file.errorString() );
    }
    return {};
}

/// The last nats.c error of this thread, or nats.c's text for @p status.
QString natsError( natsStatus status )
{
    const char* detail = nats_GetLastError( nullptr );
    return QString::fromUtf8( detail && *detail ? detail : natsStatus_GetText( status ) );
}

QString configureAuth( natsOptions* options, const AuthConfig& auth )
{
    natsStatus status = NATS_OK;
    switch ( auth.method ) {
    case AuthMethod::None:
        return {};
    case AuthMethod::UserPassword:
        status = natsOptions_SetUserInfo( options, auth.user.toUtf8().constData(),
                                          auth.secret.toUtf8().constData() );
        break;
    case AuthMethod::Token:
        status = natsOptions_SetToken( options, auth.secret.toUtf8().constData() );
        break;
    case AuthMethod::Credentials: {
        const auto problem = checkReadable( auth.file, QStringLiteral( "credentials file" ) );
        if ( !problem.isEmpty() ) {
            return problem;
        }
        status = natsOptions_SetUserCredentialsFromFiles( options, auth.file.toUtf8().constData(),
                                                          nullptr );
        break;
    }
    case AuthMethod::NKey: {
        QFile file( auth.file );
        const auto problem = checkReadable( auth.file, QStringLiteral( "NKey seed file" ) );
        if ( !problem.isEmpty() || !file.open( QIODevice::ReadOnly ) ) {
            return problem;
        }
        // As nats.c reads it: the first user seed in the file.
        static const QRegularExpression seed( QStringLiteral( "SU[A-Z2-7]{56}" ) );
        const auto match = seed.match( QString::fromLatin1( file.readAll() ) );
        const auto publicKey
            = match.hasMatch() ? userPublicKey( match.captured().toLatin1() ) : QByteArray();
        if ( publicKey.isEmpty() ) {
            return QStringLiteral( "no NKey seed in %1" ).arg( auth.file );
        }
        status = natsOptions_SetNKeyFromSeed( options, publicKey.constData(),
                                              auth.file.toUtf8().constData() );
        break;
    }
    }
    if ( status != NATS_OK ) {
        return QStringLiteral( "the credentials cannot be set: %1" ).arg( natsError( status ) );
    }
    return {};
}

QString configureTls( natsOptions* options, const TlsConfig& tls )
{
    natsStatus status = natsOptions_SetSecure( options, true );
    if ( status == NATS_OK && !tls.caFile.isEmpty() ) {
        const auto problem = checkReadable( tls.caFile, QStringLiteral( "CA file" ) );
        if ( !problem.isEmpty() ) {
            return problem;
        }
        status = natsOptions_LoadCATrustedCertificates( options, tls.caFile.toUtf8().constData() );
        if ( status != NATS_OK ) {
            return QStringLiteral( "cannot use the CA file %1: %2" )
                .arg( tls.caFile, natsError( status ) );
        }
    }
    if ( status == NATS_OK && ( !tls.certFile.isEmpty() || !tls.keyFile.isEmpty() ) ) {
        for ( const auto& [ path, what ] :
              { std::pair{ tls.certFile, QStringLiteral( "client certificate" ) },
                std::pair{ tls.keyFile, QStringLiteral( "client key file" ) } } ) {
            const auto problem = checkReadable( path, what );
            if ( !problem.isEmpty() ) {
                return problem;
            }
        }
        status = natsOptions_LoadCertificatesChain( options, tls.certFile.toUtf8().constData(),
                                                    tls.keyFile.toUtf8().constData() );
        if ( status != NATS_OK ) {
            return QStringLiteral( "cannot use the client certificate %1 with the key %2: %3" )
                .arg( tls.certFile, tls.keyFile, natsError( status ) );
        }
    }
    if ( status == NATS_OK && !tls.serverName.trimmed().isEmpty() ) {
        status = natsOptions_SetExpectedHostname( options,
                                                  tls.serverName.trimmed().toUtf8().constData() );
    }
    if ( status == NATS_OK && tls.skipVerify ) {
        status = natsOptions_SkipServerVerification( options, true );
    }
    if ( status != NATS_OK ) {
        return QStringLiteral( "TLS cannot be set up: %1" ).arg( natsError( status ) );
    }
    return {};
}

/// Whether TLS is used: switched on, or a tls:// URL.
bool usesTls( const SessionConfig& config )
{
    if ( config.tls.enabled ) {
        return true;
    }
    for ( const auto& url : serverUrls( config.url ) ) {
        if ( url.startsWith( QStringLiteral( "tls://" ), Qt::CaseInsensitive ) ) {
            return true;
        }
    }
    return false;
}

/// The host the server's certificate is checked against.
QString expectedHost( const SessionConfig& config )
{
    if ( !config.tls.serverName.trimmed().isEmpty() ) {
        return config.tls.serverName.trimmed();
    }
    const auto urls = serverUrls( config.url );
    const auto host = urls.isEmpty() ? QString() : QUrl( urls.first() ).host();
    return host.isEmpty() ? QStringLiteral( "the server's name" ) : host;
}

} // namespace

QStringList serverUrls( const QString& url )
{
    QStringList urls;
    for ( const auto& part : url.split( ',' ) ) {
        const auto trimmed = part.trimmed();
        if ( !trimmed.isEmpty() ) {
            urls << trimmed;
        }
    }
    return urls;
}

QString configureConnection( natsOptions* options, const SessionConfig& config )
{
    const auto urls = serverUrls( config.url );
    if ( urls.isEmpty() ) {
        return QStringLiteral( "no server URL given" );
    }
    std::vector<QByteArray> encoded;
    std::vector<const char*> servers;
    for ( const auto& url : urls ) {
        encoded.push_back( url.toUtf8() );
    }
    for ( const auto& url : encoded ) {
        servers.push_back( url.constData() );
    }
    natsStatus status
        = natsOptions_SetServers( options, servers.data(), static_cast<int>( servers.size() ) );
    if ( status != NATS_OK ) {
        return QStringLiteral( "the server URLs cannot be used: %1" ).arg( natsError( status ) );
    }

    auto problem = configureAuth( options, config.auth );
    if ( problem.isEmpty() && usesTls( config ) ) {
        problem = configureTls( options, config.tls );
    }
    return problem;
}

QString explainConnectFailure( const SessionConfig& config, natsStatus status,
                               const QString& detail )
{
    const auto lower = detail.toLower();
    const auto has
        = [ &lower ]( const char* text ) { return lower.contains( QLatin1String( text ) ); };

    QString explanation;
    if ( status == NATS_NOT_PERMITTED || has( "authorization violation" )
         || has( "authentication" ) ) {
        switch ( config.auth.method ) {
        case AuthMethod::None:
            explanation = QStringLiteral( "the server requires authentication: choose how to "
                                          "log in" );
            break;
        case AuthMethod::UserPassword:
            explanation = QStringLiteral( "the server refused the user name or password" );
            break;
        case AuthMethod::Token:
            explanation = QStringLiteral( "the server refused the token" );
            break;
        case AuthMethod::Credentials:
            explanation = QStringLiteral( "the server refused the credentials in %1" )
                              .arg( config.auth.file );
            break;
        case AuthMethod::NKey:
            explanation
                = QStringLiteral( "the server refused the NKey in %1" ).arg( config.auth.file );
            break;
        }
    }
    else if ( status == NATS_SECURE_CONNECTION_REQUIRED ) {
        explanation = QStringLiteral( "the server requires TLS: switch TLS on" );
    }
    else if ( status == NATS_SECURE_CONNECTION_WANTED ) {
        explanation = QStringLiteral( "the server does not offer TLS: switch TLS off" );
    }
    else if ( has( "hostname mismatch" ) || has( "ip address mismatch" ) ) {
        explanation = QStringLiteral( "the server's certificate is not for %1: set the server "
                                      "name it is for" )
                          .arg( expectedHost( config ) );
    }
    else if ( has( "unable to get local issuer certificate" ) || has( "self-signed certificate" )
              || has( "self signed certificate" ) || has( "unable to verify the first" )
              || has( "certificate verify failed" ) ) {
        // Without TLS switched on, nats.c still uses it when the server
        // requires it, but with none of the TLS settings.
        explanation = usesTls( config )
                          ? QStringLiteral( "the server's certificate is not trusted: set the CA "
                                            "file of the CA that signed it" )
                          : QStringLiteral( "the server requires TLS, and its certificate is not "
                                            "trusted: switch TLS on and set the CA file of the CA "
                                            "that signed it" );
    }
    else if ( has( "certificate required" ) || has( "unknown ca" ) || has( "bad certificate" )
              || has( "certificate unknown" ) ) {
        // The server's TLS alert (TLS 1.2, or as nats.c reads it).
        explanation
            = config.tls.certFile.isEmpty()
                  ? QStringLiteral( "the server requires a client certificate, and none is set" )
                  : QStringLiteral( "the server did not accept the client certificate %1" )
                        .arg( config.tls.certFile );
    }
    else if ( ( status == NATS_CONNECTION_CLOSED || ( status == NATS_IO_ERROR && has( "reset" ) ) )
              && usesTls( config ) ) {
        // With TLS 1.3 a server that refuses the client's certificate
        // closes the connection after the handshake, without saying why.
        explanation
            = config.tls.certFile.isEmpty()
                  ? QStringLiteral( "the server closed the TLS connection: it may require a "
                                    "client certificate, and none is set" )
                  : QStringLiteral( "the server closed the TLS connection: it may not accept the "
                                    "client certificate %1" )
                        .arg( config.tls.certFile );
    }

    if ( explanation.isEmpty() ) {
        return detail;
    }
    return QStringLiteral( "%1 (%2)" ).arg( explanation, detail );
}

} // namespace nats_monitor

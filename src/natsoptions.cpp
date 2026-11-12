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

#include "authmethods.h"
#include "nkey.h"

#include <QFile>
#include <QUrl>

#include <initializer_list>
#include <utility>
#include <vector>

namespace nats_monitor {

namespace {

// ── Files ────────────────────────────────────────────────────────────────

/// Empty if @p path can be read, else why not, naming it as @p what.  With
/// @p contents, what the file holds is read into it.
QString readFile( const QString& path, const QString& what, QByteArray* contents = nullptr )
{
    if ( path.isEmpty() ) {
        return QStringLiteral( "no %1 given" ).arg( what );
    }
    QFile file( path );
    if ( file.open( QIODevice::ReadOnly ) && contents ) {
        *contents = file.readAll();
    }
    if ( file.error() != QFileDevice::NoError ) {
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
    const auto what = QString::fromLatin1( authMethodInfo( auth.method ).file );
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
        const auto problem = readFile( auth.file, what );
        if ( !problem.isEmpty() ) {
            return problem;
        }
        status = natsOptions_SetUserCredentialsFromFiles( options, auth.file.toUtf8().constData(),
                                                          nullptr );
        break;
    }
    case AuthMethod::NKey: {
        QByteArray seedFile;
        const auto problem = readFile( auth.file, what, &seedFile );
        if ( !problem.isEmpty() ) {
            return problem;
        }
        const auto publicKey = nkeyUserPublicKey( seedFile );
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
        const auto problem = readFile( tls.caFile, QStringLiteral( "CA file" ) );
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
            const auto problem = readFile( path, what );
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

// ── What a failed connect means ──────────────────────────────────────────
// nats.c's status codes cannot tell most TLS failures apart (they are all
// NATS_SSL_ERROR, or the connection closed), so these are matched in the
// text nats.c passes on, lower case: OpenSSL's verification errors and the
// TLS alerts it reports.  Those texts depend on the OpenSSL version (e.g.
// "self signed" became "self-signed" in 3.0): check them when OpenSSL is
// updated; the [auth] tests fail when one no longer matches.

/// The server refused the credentials (its -ERR), besides NATS_NOT_PERMITTED.
constexpr std::initializer_list<const char*> kAuthRefused{ "authorization violation",
                                                           "authentication" };

/// The server's certificate is not for the name expected.
constexpr std::initializer_list<const char*> kWrongHost{ "hostname mismatch",
                                                         "ip address mismatch" };

/// The server's certificate is not signed by a CA trusted.
constexpr std::initializer_list<const char*> kUntrusted{
    "unable to get local issuer certificate", "self-signed certificate", "self signed certificate",
    "unable to verify the first", "certificate verify failed"
};

/// The server's TLS alert about the client's certificate (TLS 1.2, or as
/// nats.c reads it).
constexpr std::initializer_list<const char*> kClientCertRefused{ "certificate required",
                                                                 "unknown ca", "bad certificate",
                                                                 "certificate unknown" };

/// The TLS connection broke off after the handshake (NATS_IO_ERROR): reset
/// by the peer, or, on Windows, a failed read on a connection the server shut
/// down ("SSL_read error: (null)").
constexpr std::initializer_list<const char*> kTlsBrokenOff{ "reset", "ssl_read error",
                                                            "ssl_write error" };

/// Whether @p lower contains one of @p patterns.
bool containsAny( const QString& lower, std::initializer_list<const char*> patterns )
{
    for ( const char* pattern : patterns ) {
        if ( lower.contains( QLatin1String( pattern ) ) ) {
            return true;
        }
    }
    return false;
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

QString redactUrls( const QString& url )
{
    QStringList parts = url.split( ',' );
    for ( auto& part : parts ) {
        const auto scheme = part.indexOf( QStringLiteral( "://" ) );
        const auto start = scheme < 0 ? 0 : scheme + 3;
        auto end = part.indexOf( '/', start );
        if ( end < 0 ) {
            end = part.size();
        }
        const auto at = end > start ? part.lastIndexOf( '@', end - 1 ) : -1;
        if ( at < start ) {
            continue;
        }
        // A user name stays; a password, or a token in place of the user,
        // does not.
        const auto colon = part.indexOf( ':', start );
        const auto kept = colon >= start && colon < at ? colon + 1 - start : 0;
        part.replace( start + kept, at - start - kept, QStringLiteral( "***" ) );
    }
    return parts.join( ',' );
}

bool usesTls( const QString& url, bool tlsEnabled )
{
    if ( tlsEnabled ) {
        return true;
    }
    for ( const auto& one : serverUrls( url ) ) {
        if ( one.startsWith( QStringLiteral( "tls://" ), Qt::CaseInsensitive ) ) {
            return true;
        }
    }
    return false;
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
    if ( problem.isEmpty() && usesTls( config.url, config.tls.enabled ) ) {
        problem = configureTls( options, config.tls );
    }
    return problem;
}

QString explainConnectFailure( const SessionConfig& config, natsStatus status,
                               const QString& detail )
{
    const auto lower = detail.toLower();
    const bool tls = usesTls( config.url, config.tls.enabled );
    const auto& certFile = config.tls.certFile;

    QString explanation;
    if ( status == NATS_NOT_PERMITTED || containsAny( lower, kAuthRefused ) ) {
        const auto& method = authMethodInfo( config.auth.method );
        explanation = QString::fromLatin1( method.refused );
        if ( method.file ) {
            explanation = explanation.arg( config.auth.file );
        }
    }
    else if ( status == NATS_SECURE_CONNECTION_REQUIRED ) {
        explanation = QStringLiteral( "the server requires TLS: switch TLS on" );
    }
    else if ( status == NATS_SECURE_CONNECTION_WANTED ) {
        explanation = QStringLiteral( "the server does not offer TLS: switch TLS off" );
    }
    else if ( containsAny( lower, kWrongHost ) ) {
        explanation = QStringLiteral( "the server's certificate is not for %1: set the server "
                                      "name it is for" )
                          .arg( expectedHost( config ) );
    }
    else if ( containsAny( lower, kUntrusted ) ) {
        // Without TLS switched on, nats.c still uses it when the server
        // requires it, but with none of the TLS settings.
        explanation = tls ? QStringLiteral( "the server's certificate is not trusted: set the CA "
                                            "file of the CA that signed it" )
                          : QStringLiteral( "the server requires TLS, and its certificate is not "
                                            "trusted: switch TLS on and set the CA file of the CA "
                                            "that signed it" );
    }
    else if ( containsAny( lower, kClientCertRefused ) ) {
        explanation
            = certFile.isEmpty()
                  ? QStringLiteral( "the server requires a client certificate, and none is set" )
                  : QStringLiteral( "the server did not accept the client certificate %1" )
                        .arg( certFile );
    }
    else if ( tls
              && ( status == NATS_CONNECTION_CLOSED
                   || ( status == NATS_IO_ERROR && containsAny( lower, kTlsBrokenOff ) ) ) ) {
        // With TLS 1.3 a server that refuses the client's certificate
        // closes the connection after the handshake, without saying why.
        explanation = certFile.isEmpty()
                          ? QStringLiteral( "the server closed the TLS connection: it may require "
                                            "a client certificate, and none is set" )
                          : QStringLiteral( "the server closed the TLS connection: it may not "
                                            "accept the client certificate %1" )
                                .arg( certFile );
    }

    if ( explanation.isEmpty() ) {
        return detail;
    }
    return QStringLiteral( "%1 (%2)" ).arg( explanation, detail );
}

QString describeStatus( natsStatus status )
{
    auto text = QString::fromUtf8( natsStatus_GetText( status ) );
    const char* detail = nats_GetLastError( nullptr );
    if ( detail && *detail ) {
        text += QStringLiteral( " (%1)" ).arg( QString::fromUtf8( detail ) );
    }
    return text;
}

QString connectTo( natsOptions* options, const SessionConfig& config, natsConnection** connection )
{
    auto why = configureConnection( options, config );
    if ( why.isEmpty() ) {
        const natsStatus status = natsConnection_Connect( connection, options );
        if ( status != NATS_OK ) {
            why = explainConnectFailure( config, status, describeStatus( status ) );
        }
    }
    return why.isEmpty() ? QString()
                         : QStringLiteral( "connection to %1 failed: %2" )
                               .arg( redactUrls( config.url ), why );
}

} // namespace nats_monitor

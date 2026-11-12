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
 * @file auth_test.cpp
 * @brief BDD tests for authentication, TLS and several server URLs: a
 *        capture session against a real nats-server started with the
 *        fixture configurations (tests/auth, tests/certs).
 *
 * The main seam: each test starts a server that requires one method, a
 * session with the right or a wrong credential, and asserts on the lines
 * of the session's file and the error it reports.
 */

// First: on Windows nats.h includes winsock2.h, which must come before any
// windows.h.
#include <nats.h>

#include <catch2/catch.hpp>

#include "natsserver.h"
#include "natssession.h"
#include "sessiontest.h"
#include "testconfig.h"

#include <QFile>
#include <QRegularExpression>
#include <QTemporaryDir>

using nats_monitor::AuthMethod;
using nats_monitor::NatsSession;
using nats_monitor::SessionConfig;
using nats_test::NatsServer;
using nats_test::NatsServerOptions;
using nats_test::Publisher;
using nats_test::readLines;
using nats_test::waitFor;

namespace {

const QString kCerts = QStringLiteral( LOGSQUIRL_NATS_TEST_CERTS );
const QString kAuth = QStringLiteral( LOGSQUIRL_NATS_TEST_AUTH );

/// The content of the fixture file @p path, trimmed.
QString fixture( const QString& path )
{
    QFile file( path );
    REQUIRE( file.open( QIODevice::ReadOnly | QIODevice::Text ) );
    return QString::fromUtf8( file.readAll() ).trimmed();
}

/// A nats-server started with the configuration @p text, written to a
/// temporary file; or with the fixture configuration file @p path.
class ConfiguredServer {
public:
    static ConfiguredServer withText( const QString& text )
    {
        return ConfiguredServer( text, {} );
    }

    static ConfiguredServer withFile( const QString& path )
    {
        return ConfiguredServer( {}, path );
    }

    QString url() const
    {
        return server_->url();
    }

    int port() const
    {
        return server_->port();
    }

private:
    ConfiguredServer( const QString& text, const QString& path )
    {
        REQUIRE( dir_.isValid() );
        NatsServerOptions options;
        options.configFile = path;
        if ( path.isEmpty() ) {
            options.configFile = dir_.filePath( "server.conf" );
            QFile file( options.configFile );
            REQUIRE( file.open( QIODevice::WriteOnly | QIODevice::Text ) );
            file.write( text.toUtf8() );
        }
        server_ = std::make_unique<NatsServer>( options );
    }

    QTemporaryDir dir_;
    std::unique_ptr<NatsServer> server_;
};

/// The server's TLS block; with @p verifyClients it requires a client
/// certificate signed by the test CA.
QString tlsBlock( bool verifyClients )
{
    return QStringLiteral( "tls {\n"
                           "  cert_file: \"%1/server-cert.pem\"\n"
                           "  key_file: \"%1/server-key.pem\"\n"
                           "%2"
                           "  timeout: 5\n"
                           "}\n" )
        .arg( kCerts, verifyClients ? QStringLiteral( "  ca_file: \"%1/ca-cert.pem\"\n"
                                                      "  verify: true\n" )
                                          .arg( kCerts )
                                    : QString() );
}

SessionConfig configFor( const QString& url, const QString& subject )
{
    SessionConfig config;
    config.url = url;
    config.subscriptions = { { subject, {} } };
    return config;
}

/// A session's run: started, and either subscribed or failed.
struct Run {
    explicit Run( const SessionConfig& config )
        : session( config )
    {
        QObject::connect( &session, &NatsSession::errorOccurred,
                          [ this ]( const QString& message ) { errors << message; } );
        QObject::connect( &session, &NatsSession::finished, [ this ] { finished = true; } );
        REQUIRE( session.start() );
        REQUIRE( waitFor(
            [ this ] {
                return finished
                       || readLines( session.filePath() ).join( '\n' ).contains( " connected to " );
            },
            15000 ) );
    }

    bool subscribed() const
    {
        return !finished
               && readLines( session.filePath() ).join( '\n' ).contains( " connected to " );
    }

    /// The error reported, which the dialog shows; empty if none.
    QString error() const
    {
        return errors.join( '\n' );
    }

    /// Whether a message published on @p subject with @p configure reaches the file.
    bool captures( const QString& url, const std::function<void( natsOptions* )>& configure )
    {
        {
            Publisher publisher( url, configure );
            publisher.publish( "auth.test", "hello" );
        }
        return waitFor( [ this ] {
            const auto lines = readLines( session.filePath() );
            return !lines.isEmpty() && lines.last().endsWith( " auth.test - 5 hello" );
        } );
    }

    NatsSession session;
    QStringList errors;
    bool finished = false;
};

/// That the session of @p run is subscribed.
void requireSubscribed( const Run& run )
{
    INFO( "error: " << run.error().toStdString() );
    REQUIRE( run.subscribed() );
}

/// That the session of @p run failed and reported an error with @p text.
void requireError( const Run& run, const QString& text )
{
    INFO( "error: " << run.error().toStdString() );
    REQUIRE( run.finished );
    REQUIRE( run.error().contains( text ) );
}

/// A publisher's options for user @p user and password @p password.
std::function<void( natsOptions* )> userInfo( const char* user, const char* password )
{
    return [ = ]( natsOptions* options ) { natsOptions_SetUserInfo( options, user, password ); };
}

/// A publisher's options over TLS, trusting the test CA, with the test
/// client certificate.
void tlsPublisher( natsOptions* options )
{
    natsOptions_SetSecure( options, true );
    natsOptions_LoadCATrustedCertificates( options,
                                           ( kCerts + "/ca-cert.pem" ).toUtf8().constData() );
    natsOptions_LoadCertificatesChain( options,
                                       ( kCerts + "/client-cert.pem" ).toUtf8().constData(),
                                       ( kCerts + "/client-key.pem" ).toUtf8().constData() );
}

} // namespace

SCENARIO( "a session authenticates with a user and password", "[server][auth]" )
{
    SKIP_WITHOUT_NATS_SERVER();

    GIVEN( "a server that requires the user alice with the password s3cret" )
    {
        auto server
            = ConfiguredServer::withText( "authorization { user: alice, password: s3cret }\n" );
        auto config = configFor( server.url(), "auth.>" );
        config.auth.method = AuthMethod::UserPassword;
        config.auth.user = "alice";

        WHEN( "a session starts with them" )
        {
            config.auth.secret = "s3cret";
            Run run( config );

            THEN( "it captures what is published" )
            {
                requireSubscribed( run );
                REQUIRE( run.captures( server.url(), userInfo( "alice", "s3cret" ) ) );
            }
        }

        WHEN( "a session starts with a wrong password" )
        {
            config.auth.secret = "wrong";
            Run run( config );

            THEN( "it fails, saying the server refused the user and password" )
            {
                requireError( run, "the server refused the user name or password" );
                requireError( run, "Authorization Violation" );
                REQUIRE( readLines( run.session.filePath() )
                             .join( '\n' )
                             .contains( "the server refused the user name or password" ) );
            }
        }

        WHEN( "a session starts without credentials" )
        {
            config.auth.method = AuthMethod::None;
            Run run( config );

            THEN( "it fails, saying the server requires authentication" )
            {
                requireError( run, "the server requires authentication" );
            }
        }
    }
}

SCENARIO( "a session authenticates with a token", "[server][auth]" )
{
    SKIP_WITHOUT_NATS_SERVER();

    GIVEN( "a server that requires the token t0ken" )
    {
        auto server = ConfiguredServer::withText( "authorization { token: t0ken }\n" );
        auto config = configFor( server.url(), "auth.>" );
        config.auth.method = AuthMethod::Token;

        WHEN( "a session starts with it" )
        {
            config.auth.secret = "t0ken";
            Run run( config );

            THEN( "it captures what is published" )
            {
                requireSubscribed( run );
                REQUIRE( run.captures( server.url(), []( natsOptions* options ) {
                    natsOptions_SetToken( options, "t0ken" );
                } ) );
            }
        }

        WHEN( "a session starts with a wrong token" )
        {
            config.auth.secret = "wrong";
            Run run( config );

            THEN( "it fails, saying the server refused the token" )
            {
                requireError( run, "the server refused the token" );
            }
        }
    }
}

SCENARIO( "a session authenticates with an NKey seed file", "[server][auth]" )
{
    SKIP_WITHOUT_NATS_SERVER();

    GIVEN( "a server that knows the fixture NKey user" )
    {
        const auto publicKey = fixture( kAuth + "/nkey.pub" );
        auto server = ConfiguredServer::withText(
            QStringLiteral( "authorization { users: [ { nkey: %1 } ] }\n" ).arg( publicKey ) );
        auto config = configFor( server.url(), "auth.>" );
        config.auth.method = AuthMethod::NKey;

        WHEN( "a session starts with its seed file" )
        {
            config.auth.file = kAuth + "/nkey.seed";
            Run run( config );

            THEN( "it captures what is published" )
            {
                requireSubscribed( run );
                REQUIRE( run.captures( server.url(), [ & ]( natsOptions* options ) {
                    natsOptions_SetNKeyFromSeed( options, publicKey.toUtf8().constData(),
                                                 ( kAuth + "/nkey.seed" ).toUtf8().constData() );
                } ) );
            }
        }

        WHEN( "a session starts with the seed of another key" )
        {
            config.auth.file = kAuth + "/other.seed";
            Run run( config );

            THEN( "it fails, saying the server refused the NKey" )
            {
                requireError( run, "the server refused the NKey" );
            }
        }

        WHEN( "a session starts with a file that holds no seed" )
        {
            config.auth.file = kCerts + "/ca-cert.pem";
            Run run( config );

            THEN( "it fails, naming the file" )
            {
                requireError( run, "no NKey seed in " + config.auth.file );
            }
        }
    }
}

SCENARIO( "a session authenticates with a .creds file", "[server][auth]" )
{
    SKIP_WITHOUT_NATS_SERVER();

    GIVEN( "a server in operator mode with the fixture operator and account" )
    {
        auto server = ConfiguredServer::withFile( kAuth + "/operator.conf" );
        auto config = configFor( server.url(), "auth.>" );
        config.auth.method = AuthMethod::Credentials;

        WHEN( "a session starts with the user's .creds file" )
        {
            config.auth.file = kAuth + "/user.creds";
            Run run( config );

            THEN( "it captures what is published" )
            {
                requireSubscribed( run );
                REQUIRE( run.captures( server.url(), []( natsOptions* options ) {
                    natsOptions_SetUserCredentialsFromFiles(
                        options, ( kAuth + "/user.creds" ).toUtf8().constData(), nullptr );
                } ) );
            }
        }

        WHEN( "a session starts with the .creds file of an account the server does not know" )
        {
            config.auth.file = kAuth + "/other.creds";
            Run run( config );

            THEN( "it fails, saying the server refused the credentials file" )
            {
                requireError( run, "the server refused the credentials in" );
            }
        }

        WHEN( "a session starts with a .creds file that does not exist" )
        {
            config.auth.file = kAuth + "/missing.creds";
            Run run( config );

            THEN( "it fails, saying it cannot read the file" )
            {
                requireError( run, "cannot read the credentials file " + config.auth.file );
            }
        }
    }
}

SCENARIO( "a session connects over TLS", "[server][auth][tls]" )
{
    SKIP_WITHOUT_NATS_SERVER();

    GIVEN( "a server that requires TLS, without client certificates" )
    {
        auto server = ConfiguredServer::withText( tlsBlock( false ) );
        auto config = configFor( server.url(), "auth.>" );
        config.tls.enabled = true;

        WHEN( "a session trusts the test CA" )
        {
            config.tls.caFile = kCerts + "/ca-cert.pem";
            Run run( config );

            THEN( "it captures what is published" )
            {
                requireSubscribed( run );
                REQUIRE( run.captures( server.url(), tlsPublisher ) );
            }
        }

        WHEN( "a session trusts another CA" )
        {
            config.tls.caFile = kCerts + "/other-ca-cert.pem";
            Run run( config );

            THEN( "it fails, saying the server's certificate is not trusted" )
            {
                requireError( run, "the server's certificate is not trusted" );
            }
        }

        WHEN( "a session expects another server name" )
        {
            config.tls.caFile = kCerts + "/ca-cert.pem";
            config.tls.serverName = "nats.example.com";
            Run run( config );

            THEN( "it fails, saying the certificate is not for that name" )
            {
                requireError( run, "the server's certificate is not for nats.example.com" );
            }
        }

        WHEN( "a session skips the verification of the server's certificate" )
        {
            config.tls.skipVerify = true;
            Run run( config );

            THEN( "it captures what is published" )
            {
                requireSubscribed( run );
                REQUIRE( run.captures( server.url(), tlsPublisher ) );
            }
        }

        WHEN( "a session does not use TLS" )
        {
            config.tls.enabled = false;
            config.tls.caFile = kCerts + "/ca-cert.pem";
            Run run( config );

            THEN( "it fails, saying the server requires TLS" )
            {
                requireError( run, "the server requires TLS" );
                requireError( run, "switch TLS on" );
            }
        }

        WHEN( "the CA file does not exist" )
        {
            config.tls.caFile = kCerts + "/missing.pem";
            Run run( config );

            THEN( "it fails, naming the file" )
            {
                requireError( run, "cannot read the CA file " + config.tls.caFile );
            }
        }
    }

    GIVEN( "a server that requires a client certificate signed by the test CA" )
    {
        auto server = ConfiguredServer::withText( tlsBlock( true ) );
        auto config = configFor( server.url(), "auth.>" );
        config.tls.enabled = true;
        config.tls.caFile = kCerts + "/ca-cert.pem";

        WHEN( "a session presents the test client certificate" )
        {
            config.tls.certFile = kCerts + "/client-cert.pem";
            config.tls.keyFile = kCerts + "/client-key.pem";
            Run run( config );

            THEN( "it captures what is published" )
            {
                requireSubscribed( run );
                REQUIRE( run.captures( server.url(), tlsPublisher ) );
            }
        }

        WHEN( "a session presents no client certificate" )
        {
            Run run( config );

            THEN( "it fails, saying the server requires (or may require) a client certificate" )
            {
                // "may": with TLS 1.3 the server closes without saying why.
                requireError( run, "a client certificate, and none is set" );
            }
        }

        WHEN( "a session presents a client certificate of another CA" )
        {
            config.tls.certFile = kCerts + "/other-client-cert.pem";
            config.tls.keyFile = kCerts + "/other-client-key.pem";
            Run run( config );

            THEN( "it fails, saying the server does not (or may not) accept that certificate" )
            {
                requireError( run, "accept the client certificate " + config.tls.certFile );
            }
        }

        WHEN( "the key does not match the certificate" )
        {
            config.tls.certFile = kCerts + "/client-cert.pem";
            config.tls.keyFile = kCerts + "/other-client-key.pem";
            Run run( config );

            THEN( "it fails, naming the files" )
            {
                requireError( run, "cannot use the client certificate " + config.tls.certFile );
            }
        }
    }
}

SCENARIO( "a session connects to one of several server URLs", "[server][auth]" )
{
    SKIP_WITHOUT_NATS_SERVER();

    GIVEN( "a URL where no server listens, and one of a running server" )
    {
        int port = 0;
        {
            NatsServer gone; // a port that was free a moment ago
            port = gone.port();
        }
        NatsServer server;
        const auto urls
            = QStringLiteral( "nats://127.0.0.1:%1, %2" ).arg( port ).arg( server.url() );

        WHEN( "a session starts with both, comma-separated" )
        {
            Run run( configFor( urls, "auth.>" ) );

            THEN( "it connects to the running one and captures what is published" )
            {
                requireSubscribed( run );
                REQUIRE( readLines( run.session.filePath() )
                             .first()
                             .contains( " connected to " + server.url() ) );
                REQUIRE( run.captures( server.url(), {} ) );
            }
        }
    }
}

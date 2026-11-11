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
 * @file natsserver_test.cpp
 * @brief nats.c against a real nats-server: plain and over TLS.
 *
 * Proves that nats.c and the OpenSSL built with it are linked and work, on
 * every platform, and that the NatsServer helper the later tests build on
 * starts a usable server.
 */

#include <catch2/catch.hpp>

#include "natsserver.h"
#include "testconfig.h"

#include <QFile>
#include <QTemporaryDir>

#include <nats.h>

#include <string>

using nats_test::NatsServer;
using nats_test::NatsServerOptions;

namespace {

/// Fails the test with nats.c's description of a status that is not NATS_OK.
void requireOk( natsStatus status, const char* what )
{
    if ( status != NATS_OK ) {
        const char* detail = nats_GetLastError( nullptr ); // NULL without one
        FAIL( what << ": " << natsStatus_GetText( status ) << " " << ( detail ? detail : "" ) );
    }
}

/// Owns what a test creates through nats.c.
struct Client {
    natsOptions* options = nullptr;
    natsConnection* connection = nullptr;
    natsSubscription* subscription = nullptr;
    natsMsg* message = nullptr;

    Client()
    {
        requireOk( natsOptions_Create( &options ), "natsOptions_Create" );
        requireOk( natsOptions_SetTimeout( options, 5000 ), "natsOptions_SetTimeout" );
        requireOk( natsOptions_SetAllowReconnect( options, false ),
                   "natsOptions_SetAllowReconnect" );
    }

    ~Client()
    {
        natsMsg_Destroy( message );
        natsSubscription_Destroy( subscription );
        natsConnection_Destroy( connection );
        natsOptions_Destroy( options );
    }

    Client( const Client& ) = delete;
    Client& operator=( const Client& ) = delete;

    /// Connect, subscribe to @p subject, publish @p payload on it and wait
    /// for it to come back.
    void roundTrip( const QString& url, const char* subject, const std::string& payload )
    {
        requireOk( natsOptions_SetURL( options, url.toUtf8().constData() ), "natsOptions_SetURL" );
        requireOk( natsConnection_Connect( &connection, options ), "natsConnection_Connect" );
        requireOk( natsConnection_SubscribeSync( &subscription, connection, subject ),
                   "natsConnection_SubscribeSync" );
        requireOk( natsConnection_Publish( connection, subject, payload.data(),
                                           static_cast<int>( payload.size() ) ),
                   "natsConnection_Publish" );
        requireOk( natsConnection_Flush( connection ), "natsConnection_Flush" );
        requireOk( natsSubscription_NextMsg( &message, subscription, 5000 ),
                   "natsSubscription_NextMsg" );
    }
};

} // namespace

SCENARIO( "nats.c publishes to and receives from a nats-server", "[server]" )
{
    SKIP_WITHOUT_NATS_SERVER();

    GIVEN( "a nats-server started by the test helper" )
    {
        NatsServer server;
        REQUIRE( server.url() == QString( "nats://127.0.0.1:%1" ).arg( server.port() ) );

        WHEN( "a client subscribes to a subject and publishes one message on it" )
        {
            Client client;
            client.roundTrip( server.url(), "orders.eu.created", R"({"id":1,"total":9.5})" );

            THEN( "the client receives that message" )
            {
                REQUIRE( std::string( natsMsg_GetSubject( client.message ) )
                         == "orders.eu.created" );
                REQUIRE( std::string( natsMsg_GetData( client.message ),
                                      natsMsg_GetDataLength( client.message ) )
                         == R"({"id":1,"total":9.5})" );
            }
        }
    }
}

SCENARIO( "nats.c connects over TLS", "[server][tls]" )
{
    SKIP_WITHOUT_NATS_SERVER();

    GIVEN( "a nats-server that requires TLS with the test certificate" )
    {
        const QString certs = QStringLiteral( LOGSQUIRL_NATS_TEST_CERTS );
        QTemporaryDir dir;
        REQUIRE( dir.isValid() );
        const QString configFile = dir.filePath( "tls.conf" );
        {
            QFile file( configFile );
            REQUIRE( file.open( QIODevice::WriteOnly | QIODevice::Text ) );
            file.write( QStringLiteral( "tls {\n"
                                        "  cert_file: \"%1/server-cert.pem\"\n"
                                        "  key_file: \"%1/server-key.pem\"\n"
                                        "  timeout: 5\n"
                                        "}\n" )
                            .arg( certs )
                            .toUtf8() );
        }
        NatsServerOptions options;
        options.configFile = configFile;
        NatsServer server( options );

        WHEN( "a client that trusts the test CA connects with TLS and sends a message" )
        {
            Client client;
            requireOk( natsOptions_SetSecure( client.options, true ), "natsOptions_SetSecure" );
            requireOk( natsOptions_LoadCATrustedCertificates(
                           client.options, ( certs + "/ca-cert.pem" ).toUtf8().constData() ),
                       "natsOptions_LoadCATrustedCertificates" );
            // The certificate is for localhost; the server listens on 127.0.0.1.
            requireOk( natsOptions_SetExpectedHostname( client.options, "localhost" ),
                       "natsOptions_SetExpectedHostname" );
            client.roundTrip( server.url(), "sensors.t1", "21.5" );

            THEN( "the message comes back over the TLS connection" )
            {
                REQUIRE( std::string( natsMsg_GetData( client.message ),
                                      natsMsg_GetDataLength( client.message ) )
                         == "21.5" );
            }
        }

        WHEN( "a client that does not trust the test CA connects with TLS" )
        {
            Client client;
            requireOk( natsOptions_SetSecure( client.options, true ), "natsOptions_SetSecure" );
            requireOk( natsOptions_SetExpectedHostname( client.options, "localhost" ),
                       "natsOptions_SetExpectedHostname" );
            requireOk( natsOptions_SetURL( client.options, server.url().toUtf8().constData() ),
                       "natsOptions_SetURL" );
            const natsStatus status = natsConnection_Connect( &client.connection, client.options );

            THEN( "the certificate check refuses the connection" )
            {
                REQUIRE( status != NATS_OK );
            }
        }
    }
}

SCENARIO( "the test helper starts a server with JetStream", "[server]" )
{
    SKIP_WITHOUT_NATS_SERVER();

    GIVEN( "a nats-server started with JetStream" )
    {
        NatsServerOptions options;
        options.jetStream = true;
        NatsServer server( options );

        WHEN( "a client asks for its JetStream account information" )
        {
            Client client;
            requireOk( natsOptions_SetURL( client.options, server.url().toUtf8().constData() ),
                       "natsOptions_SetURL" );
            requireOk( natsConnection_Connect( &client.connection, client.options ),
                       "natsConnection_Connect" );
            jsCtx* js = nullptr;
            requireOk( natsConnection_JetStream( &js, client.connection, nullptr ),
                       "natsConnection_JetStream" );
            jsAccountInfo* info = nullptr;
            jsErrCode errorCode = static_cast<jsErrCode>( 0 );
            const natsStatus status = js_GetAccountInfo( &info, js, nullptr, &errorCode );
            jsAccountInfo_Destroy( info );
            jsCtx_Destroy( js );

            THEN( "JetStream answers" )
            {
                requireOk( status, "js_GetAccountInfo" );
            }
        }
    }
}

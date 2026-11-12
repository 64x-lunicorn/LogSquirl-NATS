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
 * @file profiles_test.cpp
 * @brief BDD tests for the profile store, round trips on a temporary
 *        settings file (issue #31).
 *
 * The store is opened on a file path and told which connections run by a
 * function, so it is tested without the plugin, its host or a server.  The
 * files nats.ini had before (0.1.0, and 0.2.0 before the store) are written
 * here as they were, byte for byte, and must still load.
 */

#include <catch2/catch.hpp>

#include "profiles.h"

#include <QFile>
#include <QStringList>
#include <QTemporaryDir>

using nats_monitor::AuthMethod;
using nats_monitor::ConnectionProfile;
using nats_monitor::kDefaultMaxBinaryBytes;
using nats_monitor::kDefaultMaxFileMiB;
using nats_monitor::kDefaultPendingLimits;
using nats_monitor::ProfileStore;
using nats_monitor::ReplayStart;

namespace {

/// A profile @p name capturing @p subjects from @p url.
ConnectionProfile profile( const QString& name, const QString& url = "nats://localhost:4222",
                           const QString& subjects = "orders.>" )
{
    ConnectionProfile p;
    p.name = name;
    p.url = url;
    p.subjects = subjects;
    return p;
}

/// The names of @p profiles, in their order.
QStringList namesOf( const QList<ConnectionProfile>& profiles )
{
    QStringList names;
    for ( const auto& p : profiles ) {
        names << p.name;
    }
    return names;
}

/// Counts how often a store says it changed, from its creation on.
class ChangeCounter {
public:
    explicit ChangeCounter( ProfileStore& store )
    {
        QObject::connect( &store, &ProfileStore::changed, &context_, [ this ] { ++count_; } );
    }

    int count() const
    {
        return count_;
    }

private:
    QObject context_; ///< Disconnects when the counter goes.
    int count_ = 0;
};

/// Write @p text to @p path as it is.
void writeFile( const QString& path, const QByteArray& text )
{
    QFile file( path );
    REQUIRE( file.open( QIODevice::WriteOnly | QIODevice::Truncate ) );
    REQUIRE( file.write( text ) == text.size() );
}

/// nats.ini as 0.1.0 wrote it: a profile had its server, authentication and
/// TLS settings, and the size limit was remembered on its own.
constexpr const char* kIni010 = R"([capture]
maxFileMiB=7

[profiles]
1\auth\file=
1\auth\method=user
1\auth\user=alice
1\name=prod
1\tls\caFile=/etc/nats/ca.pem
1\tls\certFile=
1\tls\enabled=true
1\tls\keyFile=
1\tls\serverName=nats.example.com
1\tls\skipVerify=false
1\url="nats://a:4222, nats://b:4222"
2\auth\file=/home/u/dev.creds
2\auth\method=creds
2\auth\user=
2\name=dev
2\tls\caFile=
2\tls\certFile=
2\tls\enabled=false
2\tls\keyFile=
2\tls\serverName=
2\tls\skipVerify=false
2\url=nats://localhost:4222
size=2
)";

/// nats.ini as written before the profile store: each profile with its
/// capture settings and its replay.
constexpr const char* kIniBeforeStore = R"([capture]
maxFileMiB=9

[profiles]
1\auth\file=
1\auth\method=user
1\auth\user=alice
1\capture\maxBinaryBytes=64
1\capture\maxFileMiB=9
1\capture\pendingBytes=33554432
1\capture\pendingMessages=5000
1\capture\subjects=orders.>\njobs.* workers
1\name=prod
1\replay\filterSubject=
1\replay\followLive=false
1\replay\lastMessages=100
1\replay\sequence=1
1\replay\start=all
1\replay\stream=
1\replay\timeUs=0
1\tls\caFile=/etc/nats/ca.pem
1\tls\certFile=
1\tls\enabled=true
1\tls\keyFile=
1\tls\serverName=nats.example.com
1\tls\skipVerify=false
1\url="nats://a:4222, nats://b:4222"
2\auth\file=/home/u/audit.creds
2\auth\method=creds
2\auth\user=
2\capture\maxBinaryBytes=256
2\capture\maxFileMiB=256
2\capture\pendingBytes=536870912
2\capture\pendingMessages=1000000
2\capture\subjects=
2\name=audit
2\replay\filterSubject=orders.eu
2\replay\followLive=true
2\replay\lastMessages=2
2\replay\sequence=1
2\replay\start=last
2\replay\stream=ORDERS
2\replay\timeUs=0
2\tls\caFile=
2\tls\certFile=
2\tls\enabled=false
2\tls\keyFile=
2\tls\serverName=
2\tls\skipVerify=false
2\url=nats://localhost:4222
size=2
)";

} // namespace

SCENARIO( "the profile store saves connections and finds them again", "[profiles]" )
{
    GIVEN( "a store on a settings file that does not exist yet" )
    {
        QTemporaryDir dir;
        const auto path = dir.filePath( "nats.ini" );
        ProfileStore store( path );

        THEN( "it has no profile, finds none, and proposes the defaults for a new one" )
        {
            REQUIRE( store.settingsFile() == path );
            REQUIRE( store.list().isEmpty() );
            REQUIRE_FALSE( store.find( "prod" ) );
            const auto fresh = store.newProfile();
            REQUIRE( fresh.name.isEmpty() );
            REQUIRE( fresh.url == "nats://localhost:4222" );
            REQUIRE( fresh.subjects.isEmpty() );
            REQUIRE( fresh.maxFileMiB == kDefaultMaxFileMiB );
        }

        WHEN( "a profile with every setting is saved" )
        {
            auto prod
                = profile( "prod", "nats://a:4222, nats://b:4222", "orders.>\njobs.* workers" );
            prod.auth.method = AuthMethod::UserPassword;
            prod.auth.user = "alice";
            prod.auth.secret = "s3cret";
            prod.tls.enabled = true;
            prod.tls.caFile = "/etc/nats/ca.pem";
            prod.tls.certFile = "/etc/nats/client.pem";
            prod.tls.keyFile = "/etc/nats/client-key.pem";
            prod.tls.serverName = "nats.example.com";
            prod.tls.skipVerify = true;
            prod.maxBinaryBytes = 64;
            prod.pendingLimits = { 5000, 32 * 1024 * 1024 };
            prod.maxFileMiB = 9;
            prod.replay.stream = "ORDERS";
            prod.replay.filterSubject = "orders.eu";
            prod.replay.start = ReplayStart::Sequence;
            prod.replay.lastMessages = 3;
            prod.replay.sequence = 42;
            prod.replay.timeUs = 1794391200000000LL;
            prod.replay.followLive = true;
            ChangeCounter changed( store );
            const auto problem = store.upsert( prod );

            THEN( "a store opened again on the file finds it, everything but the password" )
            {
                REQUIRE( problem.isEmpty() );
                REQUIRE( changed.count() == 1 );
                const auto found = ProfileStore( path ).find( "prod" );
                REQUIRE( found );
                REQUIRE( found->name == "prod" );
                REQUIRE( found->url == "nats://a:4222, nats://b:4222" );
                REQUIRE( found->auth.method == AuthMethod::UserPassword );
                REQUIRE( found->auth.user == "alice" );
                REQUIRE( found->auth.secret.isEmpty() );
                REQUIRE( found->tls.enabled );
                REQUIRE( found->tls.caFile == "/etc/nats/ca.pem" );
                REQUIRE( found->tls.certFile == "/etc/nats/client.pem" );
                REQUIRE( found->tls.keyFile == "/etc/nats/client-key.pem" );
                REQUIRE( found->tls.serverName == "nats.example.com" );
                REQUIRE( found->tls.skipVerify );
                REQUIRE( found->subjects == "orders.>\njobs.* workers" );
                REQUIRE( found->maxBinaryBytes == 64 );
                REQUIRE( found->pendingLimits.messages == 5000 );
                REQUIRE( found->pendingLimits.bytes == 32 * 1024 * 1024 );
                REQUIRE( found->maxFileMiB == 9 );
                REQUIRE( found->replay.stream == "ORDERS" );
                REQUIRE( found->replay.filterSubject == "orders.eu" );
                REQUIRE( found->replay.start == ReplayStart::Sequence );
                REQUIRE( found->replay.lastMessages == 3 );
                REQUIRE( found->replay.sequence == 42 );
                REQUIRE( found->replay.timeUs == 1794391200000000LL );
                REQUIRE( found->replay.followLive );
                QFile file( path );
                REQUIRE( file.open( QIODevice::ReadOnly ) );
                REQUIRE_FALSE( file.readAll().contains( "s3cret" ) );
            }

            THEN( "a new connection proposes its size limit, also from a store opened again" )
            {
                REQUIRE( store.newProfile().maxFileMiB == 9 );
                REQUIRE( ProfileStore( path ).newProfile().maxFileMiB == 9 );
            }
        }

        WHEN( "three profiles are saved, c, a, then b" )
        {
            for ( const auto* name : { "c", "a", "b" } ) {
                REQUIRE( store.upsert( profile( name ) ).isEmpty() );
            }

            THEN( "they are listed in the order saved, not by name" )
            {
                REQUIRE( namesOf( store.list() ) == QStringList{ "c", "a", "b" } );
                REQUIRE( namesOf( ProfileStore( path ).list() ) == QStringList{ "c", "a", "b" } );
            }

            AND_WHEN( "a is saved again with other settings" )
            {
                REQUIRE(
                    store.upsert( profile( "a", "nats://other:4222", "billing.>" ) ).isEmpty() );

                THEN( "it is replaced where it was" )
                {
                    REQUIRE( namesOf( store.list() ) == QStringList{ "c", "a", "b" } );
                    REQUIRE( store.find( "a" )->url == "nats://other:4222" );
                    REQUIRE( store.find( "a" )->subjects == "billing.>" );
                }
            }

            AND_WHEN( "a's settings are saved under the new name d: renamed in the dialog" )
            {
                auto renamed = *store.find( "a" );
                renamed.name = "d";
                REQUIRE( store.upsert( renamed ).isEmpty() );

                THEN( "d is a new profile at the end, and a stays as it was" )
                {
                    REQUIRE( namesOf( store.list() ) == QStringList{ "c", "a", "b", "d" } );
                    REQUIRE( store.find( "d" )->url == store.find( "a" )->url );
                }
            }

            AND_WHEN( "a is removed" )
            {
                ChangeCounter changed( store );
                const auto problem = store.remove( "a" );

                THEN( "the others stay, in their order, also in a store opened again" )
                {
                    REQUIRE( problem.isEmpty() );
                    REQUIRE( changed.count() == 1 );
                    REQUIRE_FALSE( store.find( "a" ) );
                    REQUIRE( namesOf( store.list() ) == QStringList{ "c", "b" } );
                    REQUIRE( namesOf( ProfileStore( path ).list() ) == QStringList{ "c", "b" } );
                }
            }

            AND_WHEN( "a connection that is not saved is removed" )
            {
                ChangeCounter changed( store );
                const auto problem = store.remove( "x" );

                THEN( "the store says there is none, and changes nothing" )
                {
                    REQUIRE( problem == "There is no saved connection x." );
                    REQUIRE( changed.count() == 0 );
                    REQUIRE( namesOf( store.list() ) == QStringList{ "c", "a", "b" } );
                }
            }
        }

        WHEN( "a profile without a name is saved" )
        {
            const auto problem = store.upsert( profile( QString() ) );

            THEN( "the store refuses it, saying why" )
            {
                REQUIRE( problem == "Enter a name for the connection to save." );
                REQUIRE( store.list().isEmpty() );
            }
        }
    }
}

SCENARIO( "the profile store does not remove a connection that runs", "[profiles]" )
{
    GIVEN( "a store whose connection prod runs, and dev does not" )
    {
        QTemporaryDir dir;
        const auto path = dir.filePath( "nats.ini" );
        ProfileStore store( path, []( const QString& name ) { return name == "prod"; } );
        REQUIRE( store.upsert( profile( "prod" ) ).isEmpty() );
        REQUIRE( store.upsert( profile( "dev" ) ).isEmpty() );

        WHEN( "prod is removed" )
        {
            ChangeCounter changed( store );
            const auto problem = store.remove( "prod" );

            THEN( "the store refuses, saying to stop it first, and keeps it" )
            {
                REQUIRE( problem == "The connection prod is capturing: stop it to delete it." );
                REQUIRE( changed.count() == 0 );
                REQUIRE( namesOf( ProfileStore( path ).list() ) == QStringList{ "prod", "dev" } );
            }
        }

        WHEN( "dev is removed" )
        {
            THEN( "it goes" )
            {
                REQUIRE( store.remove( "dev" ).isEmpty() );
                REQUIRE( namesOf( store.list() ) == QStringList{ "prod" } );
            }
        }
    }
}

SCENARIO( "the profile store says why it cannot write its file", "[profiles]" )
{
    GIVEN( "a store on a settings file in a directory that is a file" )
    {
        QTemporaryDir dir;
        writeFile( dir.filePath( "config" ), "not a directory" );
        const auto path = dir.filePath( "config/nats.ini" );
        ProfileStore store( path );

        THEN( "saving says the connection could not be saved to the file" )
        {
            REQUIRE( store.upsert( profile( "prod" ) )
                     == QStringLiteral( "The connection prod could not be saved to %1." )
                            .arg( path ) );
        }
    }

    GIVEN( "a store without a settings file: no host to say where" )
    {
        ProfileStore store( QString{} );

        THEN( "it has no profile, proposes the defaults, and saves nothing" )
        {
            REQUIRE( store.list().isEmpty() );
            REQUIRE( store.newProfile().maxFileMiB == kDefaultMaxFileMiB );
            REQUIRE_FALSE( store.upsert( profile( "prod" ) ).isEmpty() );
            REQUIRE_FALSE( store.remove( "prod" ).isEmpty() );
        }
    }
}

SCENARIO( "profiles saved by earlier versions still load", "[profiles]" )
{
    QTemporaryDir dir;
    const auto path = dir.filePath( "nats.ini" );

    GIVEN( "nats.ini as 0.1.0 wrote it, without subjects, limits or replay in a profile" )
    {
        writeFile( path, kIni010 );
        ProfileStore store( path );

        THEN( "both profiles load in their order, with the capture defaults" )
        {
            REQUIRE( namesOf( store.list() ) == QStringList{ "prod", "dev" } );
            const auto prod = store.find( "prod" );
            REQUIRE( prod );
            REQUIRE( prod->url == "nats://a:4222, nats://b:4222" );
            REQUIRE( prod->auth.method == AuthMethod::UserPassword );
            REQUIRE( prod->auth.user == "alice" );
            REQUIRE( prod->tls.enabled );
            REQUIRE( prod->tls.caFile == "/etc/nats/ca.pem" );
            REQUIRE( prod->tls.serverName == "nats.example.com" );
            REQUIRE_FALSE( prod->tls.skipVerify );
            REQUIRE( prod->subjects.isEmpty() );
            REQUIRE( prod->maxBinaryBytes == kDefaultMaxBinaryBytes );
            REQUIRE( prod->pendingLimits.messages == kDefaultPendingLimits.messages );
            REQUIRE( prod->pendingLimits.bytes == kDefaultPendingLimits.bytes );
            REQUIRE( prod->maxFileMiB == kDefaultMaxFileMiB );
            REQUIRE_FALSE( prod->replay.enabled() );
            const auto dev = store.find( "dev" );
            REQUIRE( dev );
            REQUIRE( dev->auth.method == AuthMethod::Credentials );
            REQUIRE( dev->auth.file == "/home/u/dev.creds" );
            REQUIRE_FALSE( dev->tls.enabled );
        }

        THEN( "the size limit it remembered is proposed for a new connection" )
        {
            REQUIRE( store.newProfile().maxFileMiB == 7 );
        }

        WHEN( "one is saved again and the other removed" )
        {
            auto prod = *store.find( "prod" );
            prod.subjects = "orders.>";
            REQUIRE( store.upsert( prod ).isEmpty() );
            REQUIRE( store.remove( "dev" ).isEmpty() );

            THEN( "the file holds the one left, with its subjects" )
            {
                REQUIRE( namesOf( ProfileStore( path ).list() ) == QStringList{ "prod" } );
                REQUIRE( ProfileStore( path ).find( "prod" )->subjects == "orders.>" );
                REQUIRE( ProfileStore( path ).find( "prod" )->tls.caFile == "/etc/nats/ca.pem" );
            }
        }
    }

    GIVEN( "nats.ini as written before the profile store, with a capture and a replay" )
    {
        writeFile( path, kIniBeforeStore );
        ProfileStore store( path );

        THEN( "both load with every setting, and the size limit is remembered" )
        {
            REQUIRE( namesOf( store.list() ) == QStringList{ "prod", "audit" } );
            const auto prod = store.find( "prod" );
            REQUIRE( prod->subjects == "orders.>\njobs.* workers" );
            REQUIRE( prod->maxBinaryBytes == 64 );
            REQUIRE( prod->pendingLimits.messages == 5000 );
            REQUIRE( prod->pendingLimits.bytes == 33554432 );
            REQUIRE( prod->maxFileMiB == 9 );
            REQUIRE_FALSE( prod->replay.enabled() );
            const auto audit = store.find( "audit" );
            REQUIRE( audit->auth.method == AuthMethod::Credentials );
            REQUIRE( audit->auth.file == "/home/u/audit.creds" );
            REQUIRE( audit->replay.stream == "ORDERS" );
            REQUIRE( audit->replay.filterSubject == "orders.eu" );
            REQUIRE( audit->replay.start == ReplayStart::Last );
            REQUIRE( audit->replay.lastMessages == 2 );
            REQUIRE( audit->replay.followLive );
            REQUIRE( store.newProfile().maxFileMiB == 9 );
        }
    }
}

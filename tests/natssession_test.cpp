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
 * @file natssession_test.cpp
 * @brief BDD tests for a capture session against a real nats-server.
 *
 * The main seam of the plugin: a test starts a server, starts a session,
 * publishes with nats.c and reads the lines of the session's file.  Only
 * what the file and the session's signals show is asserted.
 */

// First: on Windows nats.h includes winsock2.h, which must come before any
// windows.h.
#include <nats.h>

#include <catch2/catch.hpp>

#include "natsserver.h"
#include "natssession.h"
#include "sessiontest.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QRegularExpression>
#include <QTemporaryDir>

#include <memory>

using nats_monitor::NatsSession;
using nats_monitor::SessionConfig;
using nats_test::NatsServer;
using nats_test::NatsServerOptions;
using nats_test::Publisher;
using nats_test::readLines;
using nats_test::settle;
using nats_test::waitFor;

namespace {

SessionConfig configFor( const QString& url, const QString& subject )
{
    SessionConfig config;
    config.url = url;
    config.subject = subject;
    return config;
}

/// The marker line a session writes once it is subscribed.
QString connectedMarker( const QString& url, const QString& subject )
{
    return QStringLiteral( " \\$NATS-MONITOR - \\d+" )
           + QRegularExpression::escape(
               QStringLiteral( " connected to %1, subscribed to %2" ).arg( url, subject ) )
           + "$";
}

/// Whether some line of @p lines matches @p pattern.
bool anyMatches( const QStringList& lines, const QString& pattern )
{
    const QRegularExpression re( pattern );
    for ( const auto& line : lines ) {
        if ( re.match( line ).hasMatch() ) {
            return true;
        }
    }
    return false;
}

/// Start @p session and wait until it is subscribed.
void startAndWaitConnected( NatsSession& session, const QString& url, const QString& subject )
{
    REQUIRE( session.start() );
    REQUIRE( waitFor(
        [ & ] {
            return anyMatches( readLines( session.filePath() ), connectedMarker( url, subject ) );
        },
        10000 ) );
}

const QString kTimestamp
    = QStringLiteral( "^\\d{4}-\\d\\d-\\d\\d \\d\\d:\\d\\d:\\d\\d\\.\\d{6}Z " );

} // namespace

SCENARIO( "a published message shows up as a line in the session's file", "[server][session]" )
{
    SKIP_WITHOUT_NATS_SERVER();

    GIVEN( "a session subscribed to a subject with a wildcard" )
    {
        NatsServer server;
        NatsSession session( configFor( server.url(), "orders.*.created" ) );
        startAndWaitConnected( session, server.url(), "orders.*.created" );

        THEN( "the file is in a logsquirl-nats temporary directory" )
        {
            REQUIRE(
                QFileInfo( session.filePath() ).dir().dirName().startsWith( "logsquirl-nats-" ) );
        }

        WHEN( "a message with a reply subject and headers is published on a matching subject" )
        {
            Publisher publisher( server.url() );
            QElapsedTimer timer;
            timer.start();
            publisher.publish( "orders.eu.created", R"({"id":1,"total":9.5})", "_INBOX.ab12",
                               { { "Trace", "abc" } } );

            THEN( "it is a line of the file within a second" )
            {
                const QString expected
                    = kTimestamp
                      + QRegularExpression::escape(
                          R"(orders.eu.created _INBOX.ab12 20 [Trace=abc] {"id":1,"total":9.5})" )
                      + "$";
                REQUIRE( waitFor(
                    [ & ] { return anyMatches( readLines( session.filePath() ), expected ); },
                    1000 ) );
                REQUIRE( timer.elapsed() < 1000 );
            }
        }

        WHEN( "messages are published on a matching and on another subject" )
        {
            Publisher publisher( server.url() );
            publisher.publish( "orders.eu.cancelled", "no" );
            publisher.publish( "orders.us.created", "yes" );

            THEN( "only the matching one is captured" )
            {
                REQUIRE( waitFor(
                    [ & ] {
                        return anyMatches( readLines( session.filePath() ),
                                           kTimestamp + "orders\\.us\\.created - 3 yes$" );
                    },
                    1000 ) );
                REQUIRE_FALSE(
                    anyMatches( readLines( session.filePath() ), "orders\\.eu\\.cancelled" ) );
            }
        }

        WHEN( "many messages are published in a burst" )
        {
            Publisher publisher( server.url() );
            for ( int i = 0; i < 1000; ++i ) {
                publisher.publish( "orders.eu.created", QByteArray::number( i ) );
            }

            THEN( "each is one line, in order" )
            {
                REQUIRE( waitFor( [ & ] { return readLines( session.filePath() ).size() == 1001; },
                                  5000 ) );
                const auto lines = readLines( session.filePath() );
                for ( int i = 0; i < 1000; ++i ) {
                    REQUIRE( lines.at( i + 1 ).endsWith( QStringLiteral( " %1" ).arg( i ) ) );
                }
            }
        }
    }
}

SCENARIO( "stopping a session closes the connection and keeps the file", "[server][session]" )
{
    SKIP_WITHOUT_NATS_SERVER();

    GIVEN( "a running session that has captured a message" )
    {
        NatsServer server;
        Publisher publisher( server.url() );
        auto session = std::make_unique<NatsSession>( configFor( server.url(), ">" ) );
        startAndWaitConnected( *session, server.url(), ">" );
        const QString path = session->filePath();
        publisher.publish( "a", "before" );
        REQUIRE( waitFor( [ & ] { return anyMatches( readLines( path ), " a - 6 before$" ); } ) );

        WHEN( "it is stopped" )
        {
            session->stop();

            THEN( "it no longer runs, and the file ends with a stopped marker" )
            {
                REQUIRE_FALSE( session->isRunning() );
                REQUIRE( readLines( path ).last().endsWith( " $NATS-MONITOR - 7 stopped" ) );
            }

            AND_WHEN( "a message is published afterwards" )
            {
                publisher.publish( "a", "after" );
                settle( 300 );

                THEN( "it is not captured" )
                {
                    REQUIRE_FALSE( anyMatches( readLines( path ), "after" ) );
                }
            }

            AND_WHEN( "the file is preserved and the session destroyed" )
            {
                session->preserveFile();
                session.reset();

                THEN( "the file is still there, for the tab that shows it" )
                {
                    REQUIRE( anyMatches( readLines( path ), " a - 6 before$" ) );
                    QDir( QFileInfo( path ).path() ).removeRecursively();
                }
            }

            AND_WHEN( "the session is destroyed without preserving its file" )
            {
                session.reset();

                THEN( "its temporary directory is removed" )
                {
                    REQUIRE_FALSE( QFileInfo::exists( QFileInfo( path ).path() ) );
                }
            }
        }
    }
}

SCENARIO( "a session can be started and stopped repeatedly", "[server][session]" )
{
    SKIP_WITHOUT_NATS_SERVER();

    GIVEN( "a server" )
    {
        NatsServer server;
        Publisher publisher( server.url() );

        THEN( "start, stop, start, stop… each capture in its own file" )
        {
            QStringList paths;
            for ( int round = 0; round < 3; ++round ) {
                NatsSession session( configFor( server.url(), "round" ) );
                startAndWaitConnected( session, server.url(), "round" );
                publisher.publish( "round", QByteArray::number( round ) );
                REQUIRE( waitFor( [ & ] {
                    return anyMatches( readLines( session.filePath() ),
                                       QStringLiteral( " round - 1 %1$" ).arg( round ) );
                } ) );
                session.stop();
                REQUIRE_FALSE( session.isRunning() );
                paths << QFileInfo( session.filePath() ).path();
            }
            REQUIRE( paths.removeDuplicates() == 0 );
        }
    }
}

SCENARIO( "destroying a running session stops it cleanly", "[server][session]" )
{
    SKIP_WITHOUT_NATS_SERVER();

    GIVEN( "a running session that is receiving a stream of messages" )
    {
        NatsServer server;
        Publisher publisher( server.url() );
        QString path;
        QElapsedTimer timer;
        {
            NatsSession session( configFor( server.url(), "stream" ) );
            startAndWaitConnected( session, server.url(), "stream" );
            session.preserveFile();
            path = session.filePath();
            for ( int i = 0; i < 2000; ++i ) {
                publisher.publish( "stream", QByteArray( 100, 'x' ), {}, {}, false );
            }
            timer.start();
        } // as on plugin unload or quit

        THEN( "it ends quickly, and the file ends with the stopped marker after whole lines" )
        {
            REQUIRE( timer.elapsed() < 5000 );
            const auto lines = readLines( path );
            REQUIRE( lines.last().endsWith( " $NATS-MONITOR - 7 stopped" ) );
            for ( const auto& line : lines ) {
                REQUIRE( QRegularExpression( kTimestamp ).match( line ).hasMatch() );
            }
            QDir( QFileInfo( path ).path() ).removeRecursively();
        }
    }
}

SCENARIO( "connection problems are reported and written as marker lines", "[server][session]" )
{
    SKIP_WITHOUT_NATS_SERVER();

    GIVEN( "a URL where no server listens" )
    {
        int port = 0;
        {
            NatsServer server; // a port that was free a moment ago
            port = server.port();
        }
        const QString url = QStringLiteral( "nats://127.0.0.1:%1" ).arg( port );
        NatsSession session( configFor( url, "x" ) );
        QStringList errors;
        QObject::connect( &session, &NatsSession::errorOccurred,
                          [ &errors ]( const QString& message ) { errors << message; } );
        int finished = 0;
        QObject::connect( &session, &NatsSession::finished, [ &finished ] { ++finished; } );

        WHEN( "a session is started" )
        {
            REQUIRE( session.start() );

            THEN( "the failure is reported, written to the file, and the session ends" )
            {
                REQUIRE( waitFor( [ & ] { return finished == 1; }, 10000 ) );
                REQUIRE( errors.size() == 1 );
                REQUIRE( errors.first().contains( url ) );
                REQUIRE_FALSE( session.isRunning() );
                REQUIRE( anyMatches( readLines( session.filePath() ),
                                     kTimestamp + "\\$NATS-MONITOR - \\d+ connection to "
                                         + QRegularExpression::escape( url ) + " failed: " ) );
            }
        }
    }

    GIVEN( "a session connected to a server that then goes away and comes back" )
    {
        auto server = std::make_unique<NatsServer>();
        const int port = server->port();
        const QString url = server->url();
        NatsSession session( configFor( url, "back" ) );
        QStringList status;
        QObject::connect( &session, &NatsSession::statusChanged,
                          [ &status ]( const QString& text ) { status << text; } );
        QStringList connected;
        QObject::connect( &session, &NatsSession::connected,
                          [ &connected ]( const QString& server ) { connected << server; } );
        startAndWaitConnected( session, url, "back" );

        WHEN( "the server stops" )
        {
            server.reset();

            THEN( "a disconnected marker is written" )
            {
                REQUIRE( waitFor(
                    [ & ] {
                        return anyMatches( readLines( session.filePath() ),
                                           kTimestamp + "\\$NATS-MONITOR - 12 disconnected$" );
                    },
                    10000 ) );
                REQUIRE( session.isRunning() );
            }

            AND_WHEN( "a server starts on the same port again" )
            {
                NatsServerOptions options;
                options.port = port;
                server = std::make_unique<NatsServer>( options );

                THEN( "a reconnected marker is written, and messages arrive again" )
                {
                    REQUIRE( waitFor(
                        [ & ] {
                            return anyMatches( readLines( session.filePath() ),
                                               kTimestamp + "\\$NATS-MONITOR - \\d+ reconnected to "
                                                   + QRegularExpression::escape( url ) + "$" );
                        },
                        15000 ) );
                    Publisher publisher( url );
                    publisher.publish( "back", "again" );
                    REQUIRE( waitFor( [ & ] {
                        return anyMatches( readLines( session.filePath() ), " back - 5 again$" );
                    } ) );
                    // The dialog shows them too.
                    REQUIRE( waitFor( [ & ] { return status.size() >= 3; } ) );
                    REQUIRE( status.at( 0 ) == "connected to " + url );
                    REQUIRE( status.at( 1 ) == "disconnected" );
                    REQUIRE( status.at( 2 ) == "reconnected to " + url );
                    // Connected again, as after the first connect.
                    REQUIRE( connected == QStringList{ url, url } );
                }
            }
        }
    }
}

// ── Dropped messages ─────────────────────────────────────────────────────

namespace {

/// What a capture's file says about its messages and drops.
struct Tally {
    qint64 written = 0;      ///< Message lines on the subject.
    qint64 slowConsumer = 0; ///< The sum of dropped= of the slow-consumer markers.
    qint64 queueFull = 0;    ///< The sum of dropped= of the queue-full markers.
    int markers = 0;         ///< dropped= markers.
    int unknown = 0;         ///< Lines that say dropped= in another shape.

    qint64 dropped() const
    {
        return slowConsumer + queueFull;
    }
};

Tally tally( const QStringList& lines, const QString& subject )
{
    static const QRegularExpression marker(
        kTimestamp + "\\$NATS-MONITOR - \\d+ dropped=(\\d+) \\((slow consumer|queue full)\\)$" );
    const QRegularExpression message( kTimestamp + QRegularExpression::escape( subject ) + " " );
    Tally t;
    for ( const auto& line : lines ) {
        if ( message.match( line ).hasMatch() ) {
            ++t.written;
            continue;
        }
        const auto m = marker.match( line );
        if ( m.hasMatch() ) {
            REQUIRE( m.captured( 1 ).toLongLong() > 0 );
            ++t.markers;
            ( m.captured( 2 ) == "slow consumer" ? t.slowConsumer : t.queueFull )
                += m.captured( 1 ).toLongLong();
        }
        else if ( line.contains( "dropped=" ) ) {
            ++t.unknown;
        }
    }
    return t;
}

/// Publish @p count messages of 200 bytes on @p subject as fast as the
/// connection takes them, then wait until the server has them all.
void flood( Publisher& publisher, const char* subject, int count )
{
    const QByteArray payload( 200, 'x' );
    for ( int i = 0; i < count; ++i ) {
        publisher.publish( subject, payload, {}, {}, i == count - 1 );
    }
}

/// Wait until each of the @p published messages on @p subject is
/// accounted for: a line in the file, or counted as dropped.  Each message
/// is counted at most once, so the sum reaches @p published only once the
/// session has taken them all.  (Drops still to be marked are in
/// droppedCount() already; slow-consumer drops are counted when nats.c
/// delivers the next message, which it holds while it drops.)
void waitAccounted( const NatsSession& session, const char* subject, qint64 published )
{
    REQUIRE( waitFor(
        [ & ] {
            const auto written = tally( readLines( session.filePath() ), subject ).written;
            return written + static_cast<qint64>( session.droppedCount() ) >= published;
        },
        30000 ) );
}

} // namespace

SCENARIO( "the subscription gets the session's pending limits", "[server][session][drops]" )
{
    SKIP_WITHOUT_NATS_SERVER();

    GIVEN( "a server" )
    {
        NatsServer server;

        WHEN( "a session is started with the default configuration" )
        {
            NatsSession session( configFor( server.url(), "limits" ) );
            startAndWaitConnected( session, server.url(), "limits" );

            THEN( "the subscription holds up to 1,000,000 messages and 512 MiB" )
            {
                REQUIRE( session.pendingLimits().messages == 1000000 );
                REQUIRE( session.pendingLimits().bytes == 512 * 1024 * 1024 );
            }
        }

        WHEN( "a session is started with limits of its own" )
        {
            auto config = configFor( server.url(), "limits" );
            config.pendingLimits = { 1234, 5678 * 1024 };
            NatsSession session( config );
            startAndWaitConnected( session, server.url(), "limits" );

            THEN( "the subscription has them" )
            {
                REQUIRE( session.pendingLimits().messages == 1234 );
                REQUIRE( session.pendingLimits().bytes == 5678 * 1024 );
            }
        }
    }
}

SCENARIO( "a slow consumer's drops are counted and marked where they happen",
          "[server][session][drops]" )
{
    SKIP_WITHOUT_NATS_SERVER();

    GIVEN( "a session whose subscription may hold a single pending message" )
    {
        NatsServer server;
        Publisher publisher( server.url() );
        auto config = configFor( server.url(), "flood" );
        config.pendingLimits.messages = 1;
        NatsSession session( config );
        startAndWaitConnected( session, server.url(), "flood" );

        WHEN( "it is flooded faster than it can take the messages, and stopped" )
        {
            qint64 published = 0;
            for ( int round = 0; round < 20 && session.droppedCount() == 0; ++round ) {
                flood( publisher, "flood", 5000 );
                published += 5000;
                settle( 50 );
            }
            waitAccounted( session, "flood", published );
            session.stop();

            THEN( "the slow-consumer markers add up to the messages not written" )
            {
                const auto t = tally( readLines( session.filePath() ), "flood" );
                REQUIRE( t.unknown == 0 );
                REQUIRE( t.slowConsumer > 0 );
                REQUIRE( t.queueFull == 0 );
                REQUIRE( t.dropped() == published - t.written );
                REQUIRE( session.droppedCount() == static_cast<quint64>( t.dropped() ) );
                REQUIRE( readLines( session.filePath() ).last().endsWith( " stopped" ) );
            }
        }
    }
}

SCENARIO( "under a flood every drop is counted in exactly one marker", "[server][session][drops]" )
{
    SKIP_WITHOUT_NATS_SERVER();

    GIVEN( "a session with small pending limits and a small queue" )
    {
        NatsServer server;
        Publisher publisher( server.url() );
        auto config = configFor( server.url(), "flood" );
        config.pendingLimits.messages = 16;
        config.queueLimit = 1;
        NatsSession session( config );
        startAndWaitConnected( session, server.url(), "flood" );
        const qint64 published = 50000;

        WHEN( "it is flooded faster than it can write, and stopped once it has taken it all" )
        {
            flood( publisher, "flood", published );
            waitAccounted( session, "flood", published );
            session.stop();

            THEN( "the dropped= counts of both kinds add up to published - written, and to the "
                  "session's dropped total" )
            {
                const auto t = tally( readLines( session.filePath() ), "flood" );
                REQUIRE( t.unknown == 0 );
                REQUIRE( t.slowConsumer > 0 );
                REQUIRE( t.queueFull > 0 );
                REQUIRE( t.dropped() == published - t.written );
                REQUIRE( session.droppedCount() == static_cast<quint64>( t.dropped() ) );
            }
        }

        WHEN( "it is stopped right after the flood, still dropping" )
        {
            flood( publisher, "flood", published );
            session.stop();

            THEN( "every message published is written or counted in a marker before the stopped "
                  "marker, none lost" )
            {
                const auto lines = readLines( session.filePath() );
                const auto t = tally( lines, "flood" );
                REQUIRE( t.unknown == 0 );
                REQUIRE( t.dropped() > 0 );
                REQUIRE( t.dropped() == published - t.written );
                REQUIRE( session.droppedCount() == static_cast<quint64>( t.dropped() ) );
                // What reached the session was written or dropped from its queue.
                REQUIRE( session.receivedCount()
                         == static_cast<quint64>( t.written + t.queueFull ) );
                REQUIRE( lines.last().endsWith( " stopped" ) );
            }
        }
    }
}

SCENARIO( "when the server goes away during a flood, the drops until then are still marked",
          "[server][session][drops]" )
{
    SKIP_WITHOUT_NATS_SERVER();

    GIVEN( "a session with small pending limits, flooded, whose server then goes away" )
    {
        auto server = std::make_unique<NatsServer>();
        const int port = server->port();
        const QString url = server->url();
        auto config = configFor( url, "flood" );
        config.pendingLimits.messages = 16;
        config.queueLimit = 1;
        NatsSession session( config );
        int finished = 0;
        QObject::connect( &session, &NatsSession::finished, [ &finished ] { ++finished; } );
        startAndWaitConnected( session, url, "flood" );
        {
            Publisher publisher( url );
            flood( publisher, "flood", 50000 );
        }
        server.reset(); // while the session is still taking the flood

        // Messages the server had not sent yet are gone with it: what is
        // checked is that each message nats.c took is written or counted.
        const auto check = [ & ]( const QString& lastLine ) {
            const auto lines = readLines( session.filePath() );
            const auto t = tally( lines, "flood" );
            REQUIRE( t.unknown == 0 );
            REQUIRE( t.slowConsumer > 0 );
            REQUIRE( session.droppedCount() == static_cast<quint64>( t.dropped() ) );
            REQUIRE( session.receivedCount() == static_cast<quint64>( t.written + t.queueFull ) );
            REQUIRE( lines.last().endsWith( lastLine ) );
        };

        WHEN( "the session is stopped while disconnected" )
        {
            REQUIRE( waitFor(
                [ & ] { return anyMatches( readLines( session.filePath() ), " disconnected$" ); },
                10000 ) );
            QElapsedTimer timer;
            timer.start();
            session.stop();

            THEN( "it stops at once, and every drop is in a marker before the stopped marker" )
            {
                REQUIRE( timer.elapsed() < 3000 );
                check( " stopped" );
            }
        }

        WHEN( "the connection is then closed for good (a server on the port refuses it)" )
        {
            QTemporaryDir dir;
            REQUIRE( dir.isValid() );
            const QString configFile = dir.filePath( "auth.conf" );
            {
                QFile file( configFile );
                REQUIRE( file.open( QIODevice::WriteOnly | QIODevice::Text ) );
                file.write( "authorization { token: \"not-the-session's\" }\n" );
            }
            NatsServerOptions options;
            options.port = port;
            options.configFile = configFile;
            NatsServer refusing( options );

            THEN( "the session ends, and every drop is in a marker before the closed marker" )
            {
                REQUIRE( waitFor( [ & ] { return finished == 1; }, 30000 ) );
                check( " connection closed" );
            }
        }
    }
}

// Opt-in, not run by ctest or CI (hidden tag; no [server], which ctest
// selects): logsquirl_nats_tests "[benchmark]".  See DEVELOPER_GUIDE.md.
SCENARIO( "a session sustains 50k msgs/s at 200 B without drops", "[.benchmark]" )
{
    SKIP_WITHOUT_NATS_SERVER();

    constexpr int kRate = 50000;  // messages per second
    constexpr int kSeconds = 10;  // how long it is published
    constexpr int kPayload = 200; // bytes
    constexpr qint64 kTotal = qint64( kRate ) * kSeconds;

    GIVEN( "a session with the default limits" )
    {
        NatsServer server;
        Publisher publisher( server.url() );
        NatsSession session( configFor( server.url(), "bench" ) );
        startAndWaitConnected( session, server.url(), "bench" );

        WHEN( "50k msgs/s of 200 B are published for 10 seconds" )
        {
            const QByteArray payload( kPayload, 'x' );
            QElapsedTimer timer;
            timer.start();
            qint64 sent = 0;
            while ( sent < kTotal ) {
                const qint64 due = std::min( kTotal, timer.nsecsElapsed() * kRate / 1000000000 );
                for ( ; sent < due; ++sent ) {
                    publisher.publish( "bench", payload, {}, {}, false );
                }
                QThread::usleep( 200 );
            }
            publisher.publish( "bench", payload, {}, {}, true );
            ++sent;
            const double publishSeconds = timer.nsecsElapsed() / 1e9;
            REQUIRE(
                waitFor( [ & ] { return session.receivedCount() >= quint64( sent ); }, 30000 ) );
            const double takenSeconds = timer.nsecsElapsed() / 1e9;
            waitAccounted( session, "bench", sent );
            session.stop();

            THEN( "every message is written, nothing is dropped, and no dropped= marker" )
            {
                const auto t = tally( readLines( session.filePath() ), "bench" );
                WARN( "published " << sent << " messages of " << kPayload << " B in "
                                   << publishSeconds << " s (" << qRound( sent / publishSeconds )
                                   << " msgs/s); received in " << takenSeconds << " s; written "
                                   << t.written << ", dropped " << session.droppedCount() );
                REQUIRE( t.markers == 0 );
                REQUIRE( t.unknown == 0 );
                REQUIRE( session.droppedCount() == 0 );
                REQUIRE( t.written == sent );
            }
        }
    }
}

SCENARIO( "when the session's own queue is full, its drops are counted and marked",
          "[server][session][drops]" )
{
    SKIP_WITHOUT_NATS_SERVER();

    GIVEN( "a session whose queue to the writer holds a single message" )
    {
        NatsServer server;
        Publisher publisher( server.url() );
        auto config = configFor( server.url(), "flood" );
        config.queueLimit = 1;
        NatsSession session( config );
        startAndWaitConnected( session, server.url(), "flood" );

        WHEN( "it is flooded faster than it can write, and stopped" )
        {
            qint64 published = 0;
            for ( int round = 0; round < 20 && session.droppedCount() == 0; ++round ) {
                flood( publisher, "flood", 5000 );
                published += 5000;
                settle( 50 );
            }
            waitAccounted( session, "flood", published );
            session.stop();

            THEN( "the queue-full markers add up to the messages not written, each before the "
                  "next message written" )
            {
                const auto lines = readLines( session.filePath() );
                const auto t = tally( lines, "flood" );
                REQUIRE( t.unknown == 0 );
                REQUIRE( t.queueFull > 0 );
                REQUIRE( t.slowConsumer == 0 );
                REQUIRE( t.dropped() == published - t.written );
                REQUIRE( session.droppedCount() == static_cast<quint64>( t.dropped() ) );
                REQUIRE( session.receivedCount() == static_cast<quint64>( published ) );
                for ( int i = 0; i + 1 < lines.size(); ++i ) {
                    if ( lines.at( i ).contains( "(queue full)" ) ) {
                        INFO( lines.at( i + 1 ).toStdString() );
                        REQUIRE( ( lines.at( i + 1 ).contains( " flood - 200 " )
                                   || lines.at( i + 1 ).endsWith( " stopped" ) ) );
                    }
                }
            }
        }
    }
}

SCENARIO( "without drops no dropped marker is written, and the session counts what it received",
          "[server][session][drops]" )
{
    SKIP_WITHOUT_NATS_SERVER();

    GIVEN( "a session with the default limits" )
    {
        NatsServer server;
        Publisher publisher( server.url() );
        NatsSession session( configFor( server.url(), "calm" ) );
        startAndWaitConnected( session, server.url(), "calm" );

        WHEN( "a burst it can keep up with is published, and the session stopped" )
        {
            for ( int i = 0; i < 1000; ++i ) {
                publisher.publish( "calm", QByteArray( 200, 'x' ), {}, {}, i % 100 == 99 );
            }
            REQUIRE(
                waitFor( [ & ] { return readLines( session.filePath() ).size() == 1001; }, 5000 ) );
            session.stop();

            THEN( "every message is a line, no line says dropped=, and the counts say so" )
            {
                const auto lines = readLines( session.filePath() );
                REQUIRE( lines.size() == 1002 ); // connected, 1000 messages, stopped
                REQUIRE_FALSE( anyMatches( lines, "dropped=" ) );
                REQUIRE( session.receivedCount() == 1000 );
                REQUIRE( session.droppedCount() == 0 );
            }
        }
    }
}

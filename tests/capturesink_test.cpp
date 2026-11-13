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
 * @file capturesink_test.cpp
 * @brief BDD tests for the capture sink, without a nats-server.
 *
 * Most drive a sink that is not started: the test is its writer, with
 * writeQueued() and finish(), so what is queued, dropped and written when
 * is the test's to say.  The lines are read with the line reader
 * (capture.h); a write fails through a file of the test's (FailingFile,
 * sessiontest.h).
 */

#include <catch2/catch.hpp>

#include "capture.h"
#include "capturesink.h"
#include "lineformat.h"
#include "sessiontest.h"

#include <QDir>
#include <QFileInfo>
#include <QTemporaryDir>

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

using nats_monitor::CaptureSink;
using nats_test::Capture;
using nats_test::captureFiles;
using nats_test::endsWithNewline;
using nats_test::failingFiles;
using nats_test::LineFields;
using nats_test::liveFields;
using nats_test::markerFields;
using nats_test::readLines;
using nats_test::sizeOf;
using nats_test::waitFor;
using Kind = CaptureSink::Event::Kind;

namespace {

/// The events a sink reports, from whichever thread.
class Events {
public:
    CaptureSink::Reporter reporter()
    {
        return [ this ]( const CaptureSink::Event& event ) {
            std::lock_guard<std::mutex> lock( mutex_ );
            events_ << event;
        };
    }

    QList<CaptureSink::Event> all() const
    {
        std::lock_guard<std::mutex> lock( mutex_ );
        return events_;
    }

    /// The texts of the events of @p kind, in order.
    QStringList of( Kind kind ) const
    {
        QStringList texts;
        for ( const auto& event : all() ) {
            if ( event.kind == kind ) {
                texts << event.text;
            }
        }
        return texts;
    }

private:
    mutable std::mutex mutex_;
    QList<CaptureSink::Event> events_;
};

/// The line of a live message on the subject "s" with @p payload.
QByteArray line( const QString& payload )
{
    nats_monitor::Message message;
    message.receivedUs = nats_monitor::nowUs();
    message.subject = "s";
    message.payload = payload.toUtf8();
    return nats_monitor::formatLine( message );
}

LineFields fieldsOf( const QString& payload )
{
    return liveFields( "s", payload );
}

/// A sink's config: files nats_s.log, … in @p dir.
CaptureSink::Config configIn( const QTemporaryDir& dir, int queueLimit,
                              CaptureSink::Mode mode = CaptureSink::Mode::Drop,
                              qint64 maxFileBytes = 0 )
{
    CaptureSink::Config config;
    config.fileStem = dir.filePath( "nats_s" );
    config.maxFileBytes = maxFileBytes;
    config.queueLimit = queueLimit;
    config.mode = mode;
    return config;
}

/// "m0001", "m0002", …, padded with x to @p size bytes.
QString numbered( int i, int size )
{
    return QStringLiteral( "m%1" ).arg( i, 4, 10, QLatin1Char( '0' ) ).leftJustified( size, 'x' );
}

} // namespace

SCENARIO( "every drop is in exactly one dropped= marker, at the gap", "[sink][drops]" )
{
    GIVEN( "a live capture's sink whose queue holds two messages, not written meanwhile" )
    {
        QTemporaryDir dir;
        Events events;
        CaptureSink sink( configIn( dir, 2 ), events.reporter() );
        REQUIRE( sink.open() );

        WHEN( "four messages come, it is written, a fifth comes, nats.c drops five, a sixth "
              "comes, and the capture ends" )
        {
            for ( int i = 1; i <= 4; ++i ) {
                REQUIRE( sink.message( line( QStringLiteral( "m%1" ).arg( i ) ) ) );
            }
            const auto droppedFromQueue = sink.dropped();
            sink.writeQueued();
            REQUIRE( sink.message( line( "m5" ) ) );
            sink.dropped( 5, "slow consumer" );
            REQUIRE( sink.message( line( "m6" ) ) );
            sink.finish( "stopped" );

            THEN( "the full queue dropped the third and the fourth, without waiting" )
            {
                REQUIRE( droppedFromQueue == 2 );
            }

            THEN( "each gap has its one marker, before the next message written" )
            {
                const auto capture = Capture::read( sink.fileName() );
                REQUIRE( capture.fields()
                         == QList<LineFields>{ fieldsOf( "m1" ), fieldsOf( "m2" ),
                                               markerFields( "dropped=2 (queue full)" ),
                                               fieldsOf( "m5" ),
                                               markerFields( "dropped=5 (slow consumer)" ),
                                               fieldsOf( "m6" ), markerFields( "stopped" ) } );
                const auto drops = capture.drops();
                REQUIRE( drops.markers == 2 );
                REQUIRE( drops.queueFull == 2 );
                REQUIRE( drops.slowConsumer == 5 );
                REQUIRE( drops.unknown == 0 );
            }

            THEN( "the counts are those of the markers: received is what was taken or dropped "
                  "from the queue" )
            {
                REQUIRE( sink.dropped() == 7 );
                REQUIRE( sink.received() == 6 );
            }

            THEN( "the lines are those the session wrote before: formatLine() and formatMarker()" )
            {
                const auto lines = readLines( sink.fileName() );
                REQUIRE( lines.size() == 7 );
                const auto expected
                    = nats_monitor::formatMarker( 0, "dropped=2 (queue full)" ).mid( 27 );
                REQUIRE( lines.at( 2 ).toUtf8().mid( 27 ) == expected ); // but the timestamp
            }
        }
    }
}

SCENARIO( "drops not marked yet are written before the last marker", "[sink][drops]" )
{
    GIVEN( "a live capture's sink whose queue holds two messages" )
    {
        QTemporaryDir dir;
        Events events;
        CaptureSink sink( configIn( dir, 2 ), events.reporter() );
        REQUIRE( sink.open() );

        WHEN( "five messages come, nats.c reports a drop, and the capture is stopped" )
        {
            for ( int i = 1; i <= 5; ++i ) {
                REQUIRE( sink.message( line( QStringLiteral( "m%1" ).arg( i ) ) ) );
            }
            sink.dropped( 1, "slow consumer" );
            sink.finish( "stopped" );

            THEN( "the queue-full marker comes after what was queued, before stopped" )
            {
                REQUIRE( Capture::read( sink.fileName() ).fields()
                         == QList<LineFields>{ fieldsOf( "m1" ), fieldsOf( "m2" ),
                                               markerFields( "dropped=1 (slow consumer)" ),
                                               markerFields( "dropped=3 (queue full)" ),
                                               markerFields( "stopped" ) } );
                REQUIRE( sink.dropped() == 4 );
                REQUIRE( events.all() == QList<CaptureSink::Event>{ { Kind::Status, "stopped" } } );
            }

            AND_WHEN( "a callback still comes afterwards" )
            {
                const auto before = readLines( sink.fileName() );
                const bool taken = sink.message( line( "late" ) );
                sink.marker( "disconnected", Kind::Status );
                sink.dropped( 2, "slow consumer" );
                sink.finish( "stopped" );

                THEN( "the sink takes nothing more and writes nothing" )
                {
                    REQUIRE_FALSE( taken );
                    REQUIRE( readLines( sink.fileName() ) == before );
                    REQUIRE( sink.received() == 5 );
                    REQUIRE( events.all().size() == 1 );
                }
            }
        }

        WHEN( "the queue is full when the capture ends without a last marker" )
        {
            for ( int i = 1; i <= 3; ++i ) {
                REQUIRE( sink.message( line( QStringLiteral( "m%1" ).arg( i ) ) ) );
            }
            sink.finish();

            THEN( "the drop is marked last" )
            {
                REQUIRE( Capture::read( sink.fileName() ).fields()
                         == QList<LineFields>{ fieldsOf( "m1" ), fieldsOf( "m2" ),
                                               markerFields( "dropped=1 (queue full)" ) } );
            }
        }
    }
}

SCENARIO( "messages held back until the connected marker are written after it", "[sink][hold]" )
{
    GIVEN( "a live capture's sink whose queue holds three messages, holding them back" )
    {
        QTemporaryDir dir;
        Events events;
        CaptureSink sink( configIn( dir, 3 ), events.reporter() );
        REQUIRE( sink.open() );
        sink.holdMessages();

        WHEN( "two messages and a drop come during the round trip, and the writer runs" )
        {
            REQUIRE( sink.message( line( "m1" ) ) );
            sink.dropped( 4, "slow consumer" );
            REQUIRE( sink.message( line( "m2" ) ) );
            sink.writeQueued();

            THEN( "nothing is written yet, and they are counted" )
            {
                REQUIRE( Capture::read( sink.fileName() ).isEmpty() );
                REQUIRE( events.all().isEmpty() );
                REQUIRE( sink.received() == 2 );
                REQUIRE( sink.dropped() == 4 );
            }

            AND_WHEN( "the round trip confirms the subscriptions, and more messages come" )
            {
                sink.releaseHeld( "connected to nats://h, subscribed to s", Kind::Connected,
                                  "nats://h" );
                REQUIRE( sink.message( line( "m3" ) ) );
                sink.writeQueued();
                REQUIRE( sink.message( line( "m4" ) ) );
                sink.finish( "stopped" );

                THEN( "the marker is the first line, the held ones follow in their order" )
                {
                    REQUIRE( Capture::read( sink.fileName() ).fields()
                             == QList<LineFields>{
                                 markerFields( "connected to nats://h, subscribed to s" ),
                                 fieldsOf( "m1" ), markerFields( "dropped=4 (slow consumer)" ),
                                 fieldsOf( "m2" ), fieldsOf( "m3" ), fieldsOf( "m4" ),
                                 markerFields( "stopped" ) } );
                    REQUIRE( events.all()
                             == QList<CaptureSink::Event>{ { Kind::Connected, "nats://h" },
                                                           { Kind::Status, "stopped" } } );
                }

                THEN( "none is counted twice" )
                {
                    REQUIRE( sink.received() == 4 );
                    REQUIRE( sink.dropped() == 4 );
                }
            }

            AND_WHEN( "the round trip fails" )
            {
                sink.discardHeld();
                sink.marker( "subscribing to s failed: x", Kind::Status );
                const bool late = sink.message( line( "late" ) );
                sink.finish();

                THEN( "the failure is written, and the held messages are not, nor counted, nor "
                      "one that nats.c still delivers" )
                {
                    REQUIRE_FALSE( late );
                    REQUIRE( Capture::read( sink.fileName() ).fields()
                             == QList<LineFields>{ markerFields( "dropped=4 (slow consumer)" ),
                                                   markerFields( "subscribing to s failed: x" ) } );
                    REQUIRE( sink.received() == 0 );
                    REQUIRE( sink.dropped() == 4 );
                }
            }
        }

        WHEN( "more messages come during the round trip than the queue holds" )
        {
            for ( int i = 1; i <= 5; ++i ) {
                REQUIRE( sink.message( line( QStringLiteral( "m%1" ).arg( i ) ) ) );
            }

            AND_WHEN( "the round trip confirms the subscriptions" )
            {
                sink.releaseHeld( "connected", Kind::Connected );
                sink.writeQueued();
                REQUIRE( sink.message( line( "m6" ) ) );
                sink.finish( "stopped" );

                THEN( "the full queue dropped as it does otherwise, marked at the gap" )
                {
                    REQUIRE( Capture::read( sink.fileName() ).fields()
                             == QList<LineFields>{ markerFields( "connected" ), fieldsOf( "m1" ),
                                                   fieldsOf( "m2" ), fieldsOf( "m3" ),
                                                   markerFields( "dropped=2 (queue full)" ),
                                                   fieldsOf( "m6" ), markerFields( "stopped" ) } );
                    REQUIRE( sink.received() == 6 );
                    REQUIRE( sink.dropped() == 2 );
                }
            }

            AND_WHEN( "the round trip fails" )
            {
                sink.discardHeld();
                sink.finish( "subscribing to s failed: x" );

                THEN( "neither the held messages nor their drops are written or counted" )
                {
                    REQUIRE( Capture::read( sink.fileName() ).fields()
                             == QList<LineFields>{ markerFields( "subscribing to s failed: x" ) } );
                    REQUIRE( sink.received() == 0 );
                    REQUIRE( sink.dropped() == 0 );
                }
            }
        }

        WHEN( "the capture ends while it still holds them back" )
        {
            REQUIRE( sink.message( line( "m1" ) ) );
            sink.finish( "stopped" );

            THEN( "they are discarded, as when the round trip fails" )
            {
                REQUIRE( Capture::read( sink.fileName() ).fields()
                         == QList<LineFields>{ markerFields( "stopped" ) } );
                REQUIRE( sink.received() == 0 );
            }
        }
    }

    GIVEN( "a started sink holding messages back" )
    {
        QTemporaryDir dir;
        Events events;
        CaptureSink sink( configIn( dir, 100 ), events.reporter() );
        REQUIRE( sink.open() );
        sink.start();
        sink.holdMessages();

        WHEN( "messages come, and then the marker" )
        {
            REQUIRE( sink.message( line( "m1" ) ) );
            REQUIRE( sink.message( line( "m2" ) ) );
            sink.releaseHeld( "connected", Kind::Connected );
            REQUIRE( waitFor( [ & ] { return !events.of( Kind::Connected ).isEmpty(); } ) );
            sink.finish( "stopped" );

            THEN( "the writer wrote the marker first" )
            {
                REQUIRE( Capture::read( sink.fileName() ).fields()
                         == QList<LineFields>{ markerFields( "connected" ), fieldsOf( "m1" ),
                                               fieldsOf( "m2" ), markerFields( "stopped" ) } );
            }
        }
    }
}

SCENARIO( "a replay's sink waits for room instead of dropping", "[sink][replay]" )
{
    GIVEN( "a replay's sink whose queue holds one message, and a delivery thread of three" )
    {
        QTemporaryDir dir;
        Events events;
        CaptureSink sink( configIn( dir, 1, CaptureSink::Mode::WaitForRoom ), events.reporter() );
        REQUIRE( sink.open() );
        std::atomic<int> taken{ 0 };
        std::atomic<int> discarded{ 0 };
        std::thread delivery( [ & ] {
            for ( int i = 1; i <= 3; ++i ) {
                if ( sink.message( line( QStringLiteral( "p%1" ).arg( i ) ) ) ) {
                    ++taken;
                }
                else {
                    ++discarded;
                }
            }
        } );
        const auto waitsWith = [ & ]( quint64 received ) {
            return waitFor(
                [ & ] { return sink.waitingForRoom() == 1 && sink.received() == received; } );
        };

        THEN( "the second message waits while the first is queued" )
        {
            REQUIRE( waitsWith( 1 ) );
            REQUIRE( sink.dropped() == 0 );
            sink.finish( "stopped" ); // lets it go
            delivery.join();
        }

        WHEN( "the writer takes the queue twice" )
        {
            REQUIRE( waitsWith( 1 ) );
            sink.writeQueued();
            REQUIRE( waitsWith( 2 ) );
            sink.writeQueued();
            delivery.join();
            sink.finish( "replay complete" );

            THEN( "every message is written, in order, none dropped and no dropped= marker" )
            {
                REQUIRE( taken == 3 );
                REQUIRE( sink.dropped() == 0 );
                REQUIRE( sink.received() == 3 );
                const auto capture = Capture::read( sink.fileName() );
                REQUIRE( capture.fields()
                         == QList<LineFields>{ fieldsOf( "p1" ), fieldsOf( "p2" ), fieldsOf( "p3" ),
                                               markerFields( "replay complete" ) } );
                REQUIRE( capture.drops().markers == 0 );
            }
        }

        WHEN( "the replay is stopped while the delivery waits" )
        {
            REQUIRE( waitsWith( 1 ) );
            sink.discardMessages();
            delivery.join();
            sink.marker( "a marker still", Kind::Status );
            sink.finish( "stopped" );

            THEN( "it waits no more: the rest is discarded, neither counted nor marked" )
            {
                REQUIRE( sink.discarding() );
                REQUIRE( taken == 1 );
                REQUIRE( discarded == 2 );
                REQUIRE( sink.received() == 1 );
                REQUIRE( sink.dropped() == 0 );
                REQUIRE( Capture::read( sink.fileName() ).fields()
                         == QList<LineFields>{ fieldsOf( "p1" ), markerFields( "a marker still" ),
                                               markerFields( "stopped" ) } );
            }
        }
    }
}

SCENARIO( "a sink rotates its file at the size limit, never inside a line", "[sink][rotation]" )
{
    GIVEN( "a sink with a size limit of 1000 bytes" )
    {
        QTemporaryDir dir;
        Events events;
        CaptureSink sink( configIn( dir, 1000, CaptureSink::Mode::Drop, 1000 ), events.reporter() );
        REQUIRE( sink.open() );
        const auto first = sink.fileName();
        REQUIRE( QFileInfo( first ).fileName() == "nats_s.log" );

        WHEN( "lines of about 100 bytes, 3500 bytes in all, are written a few at a time, then "
              "one longer than the limit and one more" )
        {
            QStringList written;
            sink.marker( "connected to nats://x, subscribed to s", Kind::Connected, "nats://x" );
            for ( int i = 1; i <= 35; ++i ) {
                written << numbered( i, 60 );
                REQUIRE( sink.message( line( written.last() ) ) );
                if ( i % 4 == 0 ) {
                    sink.writeQueued();
                }
            }
            written << QString( 1500, 'L' ) << QStringLiteral( "after" );
            REQUIRE( sink.message( line( written.at( 35 ) ) ) );
            REQUIRE( sink.message( line( written.at( 36 ) ) ) );
            sink.finish( "stopped" );
            const auto files = captureFiles( first );

            THEN( "the capture goes on in nats_s.2.log, …, each reported, and the sink names the "
                  "last" )
            {
                REQUIRE( files.size() == 6 );
                REQUIRE( QFileInfo( files.at( 1 ) ).fileName() == "nats_s.2.log" );
                REQUIRE( events.of( Kind::Rotated ) == files.mid( 1 ) );
                REQUIRE( events.of( Kind::FileError ).isEmpty() );
                REQUIRE( sink.fileName() == files.last() );
            }

            THEN( "each file ends with a whole line, and was rotated only when the next line "
                  "would not have fit" )
            {
                for ( int i = 0; i < files.size(); ++i ) {
                    INFO( files.at( i ).toStdString() );
                    REQUIRE( endsWithNewline( files.at( i ) ) );
                    REQUIRE( Capture::read( files.at( i ) ).unreadable().isEmpty() );
                    if ( i + 1 < files.size() ) {
                        const auto next = readLines( files.at( i + 1 ) );
                        REQUIRE_FALSE( next.isEmpty() );
                        REQUIRE( sizeOf( files.at( i ) ) + next.first().toUtf8().size() + 1
                                 > 1000 );
                    }
                }
            }

            THEN( "only the line longer than the limit is over it, alone in its file" )
            {
                for ( const auto& file : files ) {
                    const auto capture = Capture::read( file );
                    if ( sizeOf( file ) > 1000 ) {
                        REQUIRE( capture.fields()
                                 == QList<LineFields>{ fieldsOf( QString( 1500, 'L' ) ) } );
                    }
                }
            }

            THEN( "every message is there once, in order across the files, the first file "
                  "starting connected and the last ending stopped" )
            {
                const auto capture = Capture::read( files );
                REQUIRE( capture.payloadsOn( "s" ) == written );
                REQUIRE( capture.first().markerText() == "connected to nats://x, subscribed to s" );
                REQUIRE( capture.last().fields() == markerFields( "stopped" ) );
            }
        }
    }
}

SCENARIO( "a sink's rotation that fails is retried one limit later", "[sink][rotation]" )
{
    GIVEN( "a sink with a size limit of 1000 bytes whose next file cannot be created" )
    {
        QTemporaryDir dir;
        Events events;
        CaptureSink sink( configIn( dir, 1000, CaptureSink::Mode::Drop, 1000 ), events.reporter() );
        REQUIRE( sink.open() );
        const auto first = sink.fileName();
        // A directory where the next file goes: opening it fails, on every
        // platform and also for root.
        const auto second = dir.filePath( "nats_s.2.log" );
        REQUIRE( QDir().mkdir( second ) );

        int published = 0;
        const auto write = [ & ]( int count ) {
            for ( int i = 0; i < count; ++i ) {
                REQUIRE( sink.message( line( numbered( ++published, 60 ) ) ) );
            }
            sink.writeQueued();
        };

        WHEN( "the file reaches the limit" )
        {
            write( 12 ); // about 1300 bytes

            THEN( "the failure is reported once, and the capture goes on in the same file" )
            {
                const auto errors = events.of( Kind::FileError );
                REQUIRE( errors.size() == 1 );
                REQUIRE( errors.first().contains( "nats_s.2.log" ) );
                REQUIRE( events.of( Kind::Rotated ).isEmpty() );
                REQUIRE( sizeOf( first ) > 1000 );
                REQUIRE( sink.fileName() == first );
                REQUIRE( Capture::read( first ).payloadsOn( "s" ).size() == 12 );
            }

            AND_WHEN( "the next file could be created now, and the file grows by less than the "
                      "limit since the failure" )
            {
                REQUIRE( QDir().rmdir( second ) );
                write( 4 ); // about 1700 bytes

                THEN( "it is not retried on every line" )
                {
                    REQUIRE( events.of( Kind::FileError ).size() == 1 );
                    REQUIRE_FALSE( QFileInfo::exists( second ) );
                    REQUIRE( sink.fileName() == first );
                }

                AND_WHEN( "it has grown by the limit since the failure" )
                {
                    write( 10 ); // about 2700 bytes
                    sink.finish();

                    THEN( "the capture goes on in nats_s.2.log, and nothing is lost" )
                    {
                        REQUIRE( events.of( Kind::FileError ).size() == 1 );
                        REQUIRE( captureFiles( first ) == QStringList{ first, second } );
                        REQUIRE( events.of( Kind::Rotated ) == QStringList{ second } );
                        REQUIRE( sizeOf( first ) <= 2000 );
                        const auto payloads
                            = Capture::read( QStringList{ first, second } ).payloadsOn( "s" );
                        REQUIRE( payloads.size() == 26 );
                        REQUIRE( payloads.last() == numbered( 26, 60 ) );
                    }
                }
            }
        }
    }
}

SCENARIO( "a sink reports a write failure once and takes no message after it", "[sink]" )
{
    GIVEN( "a sink whose file can be written until the disk is full" )
    {
        QTemporaryDir dir;
        Events events;
        auto failing = std::make_shared<std::atomic<bool>>( false );
        CaptureSink sink( configIn( dir, 100 ), events.reporter(), failingFiles( failing ) );
        REQUIRE( sink.open() );
        sink.marker( "connected to nats://x, subscribed to s", Kind::Connected, "nats://x" );
        REQUIRE( sink.message( line( "m1" ) ) );
        sink.writeQueued();

        WHEN( "the writes fail from then on, over several batches, and the capture ends" )
        {
            failing->store( true );
            REQUIRE( sink.message( line( "m2" ) ) );
            REQUIRE( sink.message( line( "m3" ) ) );
            sink.writeQueued();
            sink.marker( "disconnected", Kind::Status );
            const bool taken = sink.message( line( "m4" ) );
            sink.writeQueued();
            sink.finish( "stopped" );

            THEN( "the failure is reported once, as the end of the capture, the events still "
                  "are, and nothing more is written" )
            {
                const auto errors = events.of( Kind::WriteFailed );
                REQUIRE( errors.size() == 1 );
                REQUIRE( errors.first().startsWith( "Cannot write to " + sink.fileName() ) );
                REQUIRE( errors.first().contains( "No space left on device" ) );
                REQUIRE( events.of( Kind::FileError ).isEmpty() );
                REQUIRE( events.of( Kind::Status ) == QStringList{ "disconnected", "stopped" } );
                REQUIRE( events.of( Kind::Connected ) == QStringList{ "nats://x" } );
                REQUIRE( Capture::read( sink.fileName() ).payloadsOn( "s" )
                         == QStringList{ "m1" } );
            }

            THEN( "a message after the failure is discarded, uncounted" )
            {
                REQUIRE_FALSE( taken );
                REQUIRE( sink.received() == 3 );
                REQUIRE( sink.discarding() );
            }
        }
    }

    GIVEN( "a replay's started sink, full, whose writes fail" )
    {
        QTemporaryDir dir;
        Events events;
        auto failing = std::make_shared<std::atomic<bool>>( true );
        CaptureSink sink( configIn( dir, 1, CaptureSink::Mode::WaitForRoom ), events.reporter(),
                          failingFiles( failing ) );
        REQUIRE( sink.open() );

        WHEN( "a delivery waits for room when the writer fails" )
        {
            std::atomic<int> taken{ 0 };
            std::thread delivery( [ & ] {
                for ( int i = 1; i <= 1000; ++i ) {
                    if ( sink.message( line( QStringLiteral( "m%1" ).arg( i ) ) ) ) {
                        ++taken;
                    }
                }
            } );
            sink.start();
            delivery.join(); // it would hang, had the failure not let it go

            THEN( "it is let go, and what comes after is discarded" )
            {
                REQUIRE( events.of( Kind::WriteFailed ).size() == 1 );
                REQUIRE( taken.load() < 1000 );
                REQUIRE( sink.received() == static_cast<quint64>( taken.load() ) );
                sink.finish();
            }
        }
    }
}

SCENARIO( "a sink's writer thread writes what any thread queues", "[sink]" )
{
    GIVEN( "a started sink" )
    {
        QTemporaryDir dir;
        Events events;
        CaptureSink sink( configIn( dir, 100000 ), events.reporter() );
        REQUIRE( sink.open() );
        sink.start();

        WHEN( "a marker and 1000 messages from two threads are queued, and the capture ends" )
        {
            sink.marker( "connected to nats://x, subscribed to s", Kind::Connected, "nats://x" );
            const auto deliver = [ & ]( const QString& prefix ) {
                for ( int i = 0; i < 500; ++i ) {
                    sink.message( line( prefix + QString::number( i ) ) );
                }
            };
            std::thread a( deliver, QStringLiteral( "a" ) );
            std::thread b( deliver, QStringLiteral( "b" ) );
            a.join();
            b.join();
            sink.finish( "stopped" );

            THEN( "each is written once, each thread's in order, between the first and the last "
                  "marker, and the events are reported in order" )
            {
                const auto capture = Capture::read( sink.fileName() );
                REQUIRE( capture.size() == 1002 );
                REQUIRE( capture.first().markerText() == "connected to nats://x, subscribed to s" );
                REQUIRE( capture.last().fields() == markerFields( "stopped" ) );
                QStringList fromA;
                QStringList fromB;
                for ( const auto& payload : capture.payloadsOn( "s" ) ) {
                    ( payload.startsWith( 'a' ) ? fromA : fromB ) << payload.mid( 1 );
                }
                REQUIRE( fromA.size() == 500 );
                REQUIRE( fromB.size() == 500 );
                for ( int i = 0; i < 500; ++i ) {
                    REQUIRE( fromA.at( i ) == QString::number( i ) );
                    REQUIRE( fromB.at( i ) == QString::number( i ) );
                }
                REQUIRE( events.all()
                         == QList<CaptureSink::Event>{ { Kind::Connected, "nats://x" },
                                                       { Kind::Status, "stopped" } } );
                REQUIRE( sink.received() == 1000 );
            }
        }
    }
}

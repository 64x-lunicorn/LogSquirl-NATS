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
 * @file capturesink.cpp
 * @brief Implementation of CaptureSink: queue, drops, writer, rotation.
 */

#include "capturesink.h"

#include "lineformat.h"

#include <QFileInfo>

#include <algorithm>
#include <utility>

namespace nats_monitor {

namespace {

/// File @p number of a capture: <stem>.log, <stem>.2.log, <stem>.3.log, …
QString numberedPath( const QString& stem, int number )
{
    return number == 1 ? stem + QStringLiteral( ".log" )
                       : QStringLiteral( "%1.%2.log" ).arg( stem ).arg( number );
}

} // namespace

// ── The files ───────────────────────────────────────────────────────────

/// The numbered files of a capture, written one after the other: the file
/// being written, and when to go on in the next (capturesink.h, ROTATION).
class CaptureSink::RotatingFile {
public:
    /// What rotateBefore() did.
    struct Rotation {
        QString path;  ///< The next file, if the capture goes on there.
        QString error; ///< What failed, if anything.
    };

    /// The files <@p stem>.log, <@p stem>.2.log, … of at most @p maxBytes
    /// each (0 for no limit), opened by @p files.
    RotatingFile( const QString& stem, qint64 maxBytes, FileFactory files )
        : stem_( stem )
        , maxBytes_( maxBytes )
        , rotateAt_( maxBytes )
        , files_( std::move( files ) )
    {
    }

    /// Open the last file there is, or the first.
    bool open()
    {
        while ( QFileInfo( numberedPath( stem_, number_ + 1 ) ).isFile() ) {
            ++number_;
        }
        file_ = files_( numberedPath( stem_, number_ ) );
        if ( !file_->open( QIODevice::WriteOnly | QIODevice::Append ) ) {
            return false;
        }
        bytes_ = file_->size();
        return true;
    }

    /// Go on in the next file if a line of @p lineBytes would take this one
    /// over the size limit.  A line longer than the limit is written whole,
    /// into a file of its own: an empty file is never rotated.  Should the
    /// next file not open, the next try is one limit later: asking on every
    /// line would report the failure on every line.
    Rotation rotateBefore( qint64 lineBytes )
    {
        if ( !file_ || maxBytes_ <= 0 || bytes_ == 0 || bytes_ + lineBytes <= rotateAt_ ) {
            return {};
        }
        const auto path = numberedPath( stem_, number_ + 1 );
        auto next = files_( path );
        // NewOnly: an earlier file is never truncated.
        if ( !next->open( QIODevice::WriteOnly | QIODevice::NewOnly ) ) {
            rotateAt_ = bytes_ + maxBytes_;
            return { {},
                     QStringLiteral(
                         "Cannot start the next file %1: %2; the session goes on in %3." )
                         .arg( path, next->errorString(), fileName() ) };
        }
        Rotation rotation{ path, {} };
        if ( !file_->flush() ) {
            rotation.error = writeError(); // lost in the old file; the new one is fine
        }
        file_->close();
        file_ = std::move( next );
        ++number_;
        bytes_ = 0;
        rotateAt_ = maxBytes_;
        return rotation;
    }

    /// Write @p line whole; false if it could not be (or no file was opened).
    bool write( const QByteArray& line )
    {
        if ( !file_ || file_->write( line ) != line.size() ) {
            return false;
        }
        bytes_ += line.size();
        return true;
    }

    bool flush()
    {
        return file_ && file_->flush();
    }

    void close()
    {
        if ( file_ ) {
            file_->close();
        }
    }

    /// The file being written; empty before open().
    QString fileName() const
    {
        return file_ ? file_->fileName() : QString();
    }

    QString errorString() const
    {
        return file_ ? file_->errorString() : QString();
    }

    /// That the file being written cannot be written, and why.
    QString writeError() const
    {
        return QStringLiteral( "Cannot write to %1: %2" ).arg( fileName(), errorString() );
    }

private:
    const QString stem_;    ///< The first file's path without ".log".
    const qint64 maxBytes_; ///< 0: no limit.
    qint64 rotateAt_;       ///< The limit, or one limit beyond where a rotation failed.
    const FileFactory files_;
    int number_ = 1;   ///< Of the file being written: 1, 2, 3, …
    qint64 bytes_ = 0; ///< In the file being written.
    std::unique_ptr<QFile> file_;
};

// ── CaptureSink ─────────────────────────────────────────────────────────

CaptureSink::CaptureSink( const Config& config, Reporter reporter, FileFactory files )
    : config_( config )
    , reporter_( std::move( reporter ) )
    , file_( std::make_unique<RotatingFile>(
          config.fileStem, config.maxFileBytes,
          files ? std::move( files )
                : []( const QString& path ) { return std::make_unique<QFile>( path ); } ) )
{
}

CaptureSink::~CaptureSink()
{
    finish();
}

bool CaptureSink::open()
{
    return file_->open();
}

QString CaptureSink::fileName() const
{
    return file_->fileName();
}

QString CaptureSink::errorString() const
{
    return file_->errorString();
}

void CaptureSink::start()
{
    thread_ = std::thread( [ this ] { run(); } );
}

bool CaptureSink::message( QByteArray line )
{
    {
        std::unique_lock<std::mutex> lock( mutex_ );
        if ( config_.mode == Mode::WaitForRoom && queuedMessages_ >= config_.queueLimit ) {
            ++waiting_;
            room_.wait( lock, [ this ] {
                return queuedMessages_ < config_.queueLimit || finishing_ || discarding_.load();
            } );
            --waiting_;
        }
        if ( finishing_ || discarding_.load() ) {
            return false; // ended, or a replay stopped: the stream keeps it
        }
        received_.fetch_add( 1, std::memory_order_relaxed );
        if ( queuedMessages_ >= config_.queueLimit ) {
            ++queueFullUnmarked_;
            dropped_.fetch_add( 1 );
            return true;
        }
        markQueueFullLocked();
        enqueueMessageLocked( std::move( line ) );
        ++queuedMessages_;
    }
    wake_.notify_all();
    return true;
}

void CaptureSink::marker( const QString& text, Event::Kind kind, const QString& eventText )
{
    {
        std::lock_guard<std::mutex> lock( mutex_ );
        if ( finishing_ ) {
            return;
        }
        enqueueLocked( formatMarker( nowUs(), text.toUtf8() ),
                       { kind, eventText.isEmpty() ? text : eventText } );
    }
    wake_.notify_all();
}

void CaptureSink::dropped( quint64 count, const char* reason )
{
    {
        std::lock_guard<std::mutex> lock( mutex_ );
        if ( finishing_ || count == 0 ) {
            return;
        }
        dropped_.fetch_add( count );
        enqueueLocked( formatMarker( nowUs(), "dropped=" + QByteArray::number( count ) + " ("
                                                  + reason + ")" ) );
    }
    wake_.notify_all();
}

void CaptureSink::discardMessages()
{
    {
        std::lock_guard<std::mutex> lock( mutex_ );
        discarding_ = true;
    }
    room_.notify_all();
}

void CaptureSink::holdMessages()
{
    std::lock_guard<std::mutex> lock( mutex_ );
    holding_ = true;
}

void CaptureSink::releaseHeld( const QString& text, Event::Kind kind, const QString& eventText )
{
    {
        std::lock_guard<std::mutex> lock( mutex_ );
        if ( finishing_ ) {
            return;
        }
        const auto held = queue_.size();
        enqueueLocked( formatMarker( nowUs(), text.toUtf8() ),
                       { kind, eventText.isEmpty() ? text : eventText } );
        if ( holding_ ) {
            // The writer took nothing meanwhile: the front is before all that was held.
            std::rotate( queue_.begin(), queue_.begin() + static_cast<std::ptrdiff_t>( held ),
                         queue_.end() );
            holding_ = false;
        }
    }
    wake_.notify_all();
}

void CaptureSink::discardHeld()
{
    {
        std::lock_guard<std::mutex> lock( mutex_ );
        discardHeldLocked();
        discarding_ = true; // what nats.c delivers still was not confirmed either
    }
    wake_.notify_all();
    room_.notify_all();
}

void CaptureSink::discardHeldLocked()
{
    if ( !holding_ ) {
        return;
    }
    holding_ = false;
    const auto messages = static_cast<quint64>( queuedMessages_ );
    queue_.erase( std::remove_if( queue_.begin(), queue_.end(),
                                  []( const Item& item ) { return item.message; } ),
                  queue_.end() );
    queuedMessages_ = 0;
    received_.fetch_sub( messages + queueFullUnmarked_ );
    dropped_.fetch_sub( queueFullUnmarked_ );
    queueFullUnmarked_ = 0;
    room_.notify_all();
}

int CaptureSink::waitingForRoom() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    return waiting_;
}

void CaptureSink::writeQueued()
{
    std::vector<Item> batch;
    {
        std::lock_guard<std::mutex> lock( mutex_ );
        if ( holding_ ) {
            return;
        }
        batch = takeLocked();
    }
    write( batch );
}

void CaptureSink::finish( const QString& text, Event::Kind kind )
{
    {
        std::lock_guard<std::mutex> lock( mutex_ );
        if ( finishing_ ) {
            return;
        }
        discardHeldLocked(); // never confirmed
        // Drops no message after them marks yet.
        markQueueFullLocked();
        if ( !text.isEmpty() ) {
            enqueueLocked( formatMarker( nowUs(), text.toUtf8() ), { kind, text } );
        }
        finishing_ = true;
    }
    wake_.notify_all();
    room_.notify_all();
    if ( thread_.joinable() ) {
        thread_.join(); // it writes what is queued and ends
    }
    else {
        writeQueued();
    }
    file_->close();
}

void CaptureSink::enqueueLocked( QByteArray line )
{
    line.append( '\n' );
    queue_.push_back( { std::move( line ), false, false, {} } );
}

void CaptureSink::enqueueMessageLocked( QByteArray line )
{
    line.append( '\n' );
    queue_.push_back( { std::move( line ), true, false, {} } );
}

void CaptureSink::enqueueLocked( QByteArray line, const Event& event )
{
    line.append( '\n' );
    queue_.push_back( { std::move( line ), false, true, event } );
}

void CaptureSink::markQueueFullLocked()
{
    if ( queueFullUnmarked_ > 0 ) {
        enqueueLocked( formatMarker( nowUs(), "dropped=" + QByteArray::number( queueFullUnmarked_ )
                                                  + " (queue full)" ) );
        queueFullUnmarked_ = 0;
    }
}

std::vector<CaptureSink::Item> CaptureSink::takeLocked()
{
    std::vector<Item> batch;
    batch.swap( queue_ );
    queuedMessages_ = 0;
    room_.notify_all();
    return batch;
}

void CaptureSink::run()
{
    for ( ;; ) {
        std::vector<Item> batch;
        bool finishing = false;
        {
            std::unique_lock<std::mutex> lock( mutex_ );
            wake_.wait( lock, [ this ] { return ( !queue_.empty() && !holding_ ) || finishing_; } );
            batch = takeLocked();
            finishing = finishing_; // then nothing more is queued: this is the last batch
        }
        write( batch );
        if ( finishing ) {
            return;
        }
    }
}

void CaptureSink::write( const std::vector<Item>& batch )
{
    for ( const auto& item : batch ) {
        if ( !writeFailed_ ) {
            rotateBefore( item.line.size() );
        }
        if ( !writeFailed_ && !file_->write( item.line ) ) {
            fail();
        }
        if ( item.reports ) {
            report( item.event );
        }
    }
    if ( !writeFailed_ && !batch.empty() && !file_->flush() ) {
        fail();
    }
}

void CaptureSink::rotateBefore( qint64 lineBytes )
{
    const auto rotation = file_->rotateBefore( lineBytes );
    if ( !rotation.error.isEmpty() ) {
        report( { Event::Kind::FileError, rotation.error } );
    }
    if ( !rotation.path.isEmpty() ) {
        report( { Event::Kind::Rotated, rotation.path } );
    }
}

void CaptureSink::fail()
{
    writeFailed_ = true;
    discardMessages(); // received() counts nothing that would not be written
    report( { Event::Kind::WriteFailed, file_->writeError() } );
}

void CaptureSink::report( const Event& event )
{
    if ( reporter_ ) {
        reporter_( event );
    }
}

} // namespace nats_monitor

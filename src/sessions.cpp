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
 * @file sessions.cpp
 * @brief Implementation of the captures, one session per saved connection.
 */

#include "sessions.h"

#include "hostadapter.h"
#include "logsquirl_plugin_api.h"
#include "natsoptions.h"
#include "natssession.h"
#include "profiles.h"
#include "replay.h"

#include <QTimer>

#include <chrono>
#include <utility>

namespace nats_monitor {

namespace {

/// How often the counts are sampled.
constexpr int kSampleMs = 1000;

/// What a session of @p config does, for the notifications and the log: "capture of
/// orders.>" or "replay of stream ORDERS, the last 10 messages".
QString describeSession( const SessionConfig& config )
{
    return config.replay.enabled()
               ? QStringLiteral( "replay of %1" ).arg( describeReplay( config.replay ) )
               : QStringLiteral( "capture of %1" )
                     .arg( describeSubscriptions( config.subscriptions ) );
}

/// The capture of @p config reached its size limit and goes on in @p path:
/// open it in a new followed tab on @p host, and log why.  Not notified, as
/// in LogSquirl-Serial: a busy capture rotates often.
void openRotatedFile( HostAdapter& host, const SessionConfig& config, const QString& path )
{
    host.openFollowed( path );
    host.log( LOGSQUIRL_LOG_INFO,
              QStringLiteral( "NATS Monitor: the %1 reached its size limit of %2 MiB; "
                              "it goes on in %3." )
                  .arg( describeSession( config ) )
                  .arg( config.maxFileBytes / kBytesPerMiB )
                  .arg( path ) );
}

/// The steady clock, in milliseconds.
qint64 steadyMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch() )
        .count();
}

/// Log @p message about the session described as @p description to
/// @p host as a warning, and show it in a host notification.
void reportError( HostAdapter& host, const QString& description, const QString& message )
{
    const auto text = QStringLiteral( "NATS Monitor (%1): %2" ).arg( description, message );
    host.log( LOGSQUIRL_LOG_WARNING, text );
    host.notify( text );
}

} // namespace

Sessions::Sessions( HostAdapter& host, const QString& tempRoot, QObject* parent, Clock clock )
    : QObject( parent )
    , host_( host )
    , tempRoot_( tempRoot )
    , clock_( clock ? std::move( clock ) : Clock( steadyMs ) )
    , timer_( new QTimer( this ) )
{
    // The timer runs in the host's event loop: nothing may throw into it.
    connect( timer_, &QTimer::timeout, this,
             [ this ] { guarded( host_, "counting the sessions", [ this ] { sample(); } ); } );
    timer_->start( kSampleMs );
}

Sessions::~Sessions()
{
    stopAll();
}

bool Sessions::start( const ConnectionProfile& profile )
{
    const auto name = profile.name;
    auto& entry = entries_[ name ];
    if ( entry.session ) {
        return true;
    }
    entry.status = {};
    if ( const auto problem = startProblem( profile ); !problem.isEmpty() ) {
        entry.status.apply( { Transition::Kind::Failed, problem, {} } );
        Q_EMIT changed( name );
        return false;
    }
    const auto config = sessionConfig( profile );
    if ( config.tls.skipVerify ) {
        host_.log(
            LOGSQUIRL_LOG_WARNING,
            QStringLiteral( "NATS Monitor: the certificate of %1 is not verified (insecure)." )
                .arg( redactUrls( config.url ) ) );
    }

    const auto description = describeSession( config );
    auto* session = new NatsSession( config, tempRoot_, this );
    // The session's signals come through the host's event loop: nothing
    // may throw into it.
    connect( session, &NatsSession::transitioned, this,
             [ this, name, description ]( const Transition& transition ) {
                 guarded( host_, "showing a session", [ this, &name, &description, &transition ] {
                     onTransition( name, transition );
                     if ( transition.kind == Transition::Kind::Failed ) {
                         reportError( host_, description, transition.detail );
                     }
                     else if ( transition.kind == Transition::Kind::Completed ) {
                         host_.notify( QStringLiteral( "NATS %1 complete." ).arg( description ) );
                     }
                 } );
             } );
    connect( session, &NatsSession::errorOccurred, this,
             [ this, name, description ]( const QString& message ) {
                 guarded( host_, "reporting a session error",
                          [ this, &name, &description, &message ] {
                              entries_[ name ].status.errorReported( message );
                              Q_EMIT changed( name );
                              reportError( host_, description, message );
                          } );
             } );
    // In the session's context, not this one's: release() disconnects this
    // object, and a file the session goes on in while it stops still gets
    // its tab (the queued signal comes before the session's deleteLater()).
    // The host adapter outlives Sessions, and so its sessions.
    connect( session, &NatsSession::fileRotated, session,
             [ host = &host_, session ]( const QString& path ) {
                 guarded( *host, "opening the next file", [ host, session, &path ] {
                     openRotatedFile( *host, session->config(), path );
                 } );
             } );
    connect( session, &NatsSession::fileRotated, this, [ this, name ]( const QString& path ) {
        guarded( host_, "showing the next file", [ this, &name, &path ] {
            entries_[ name ].status.rotated( path );
            Q_EMIT changed( name );
        } );
    } );

    // Its transitions find it: Connecting, or Failed, which lets go of it.
    entries_[ name ].session = session;
    if ( !session->start() ) {
        return false;
    }

    host_.openFollowed( session->filePath() );
    host_.notify(
        QStringLiteral( "NATS %1 started on %2." ).arg( description, redactUrls( config.url ) ) );
    return true;
}

void Sessions::stop( const QString& name, bool notify )
{
    const auto it = entries_.find( name );
    if ( it == entries_.end() || !it->session ) {
        return;
    }
    auto* session = it->session;
    const auto description = describeSession( session->config() );
    session->stop(); // its Stopped transition lets go of it
    if ( notify ) {
        host_.notify( QStringLiteral( "NATS %1 stopped." ).arg( description ) );
    }
}

void Sessions::stopAll( bool notify )
{
    // Stopped one after the other: each waits for its writer.
    const auto names = entries_.keys();
    for ( const auto& name : names ) {
        stop( name, notify );
    }
}

bool Sessions::isRunning( const QString& name ) const
{
    const auto it = entries_.constFind( name );
    return it != entries_.constEnd() && it->session;
}

bool Sessions::anyRunning() const
{
    for ( const auto& entry : entries_ ) {
        if ( entry.session ) {
            return true;
        }
    }
    return false;
}

SessionStatus Sessions::status( const QString& name ) const
{
    const auto it = entries_.constFind( name );
    return it == entries_.constEnd() ? SessionStatus{} : it->status.status();
}

void Sessions::sample()
{
    QStringList moved;
    for ( auto it = entries_.begin(); it != entries_.end(); ++it ) {
        if ( !it->session ) {
            continue;
        }
        const auto before = it->status.status();
        sampleCounts( *it );
        const auto& after = it->status.status();
        if ( after.received != before.received || after.dropped != before.dropped
             || after.rate != before.rate ) {
            moved << it.key();
        }
    }
    for ( const auto& name : std::as_const( moved ) ) {
        Q_EMIT changed( name );
    }
}

void Sessions::onTransition( const QString& name, const Transition& transition )
{
    auto& entry = entries_[ name ];
    if ( !entry.session ) {
        return; // let go of already
    }
    switch ( transition.kind ) {
    case Transition::Kind::Connecting:
        entry.status.apply( transition );
        entry.status.opened( entry.session->filePath() );
        sampleCounts( entry ); // the rate counts from here
        break;
    case Transition::Kind::Running:
    case Transition::Kind::Reconnecting:
        entry.status.apply( transition );
        break;
    case Transition::Kind::Failed:
    case Transition::Kind::Completed:
    case Transition::Kind::Stopped:
        release( entry ); // its last counts, before the end sets the rate to 0
        entry.status.apply( transition );
        break;
    }
    Q_EMIT changed( name );
}

void Sessions::sampleCounts( Entry& entry )
{
    if ( entry.session ) {
        entry.status.sample( entry.session->receivedCount(), entry.session->droppedCount(),
                             clock_() );
    }
}

void Sessions::release( Entry& entry )
{
    auto* session = entry.session;
    sampleCounts( entry );
    entry.session = nullptr;
    if ( !entry.status.status().filePath.isEmpty() ) {
        session->preserveFile(); // for its tab
    }
    session->disconnect( this );
    session->deleteLater();
}

} // namespace nats_monitor

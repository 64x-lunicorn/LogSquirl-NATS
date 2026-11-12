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

#include "natsoptions.h"
#include "natssession.h"
#include "plugin.h"
#include "profiles.h"

namespace nats_monitor {

namespace {

/// Open @p path in a new followed tab.
void openFollowed( const QString& path )
{
    if ( g_state.api && g_state.handle ) {
        g_state.api->open_file( g_state.handle, path.toUtf8().constData(), 1 );
    }
}

/// What a session of @p config does, for the notifications: "capture of
/// orders.>" or "replay of stream ORDERS, the last 10 messages".
QString describeCapture( const SessionConfig& config )
{
    return config.replay.enabled()
               ? QStringLiteral( "replay of %1" ).arg( describeReplay( config.replay ) )
               : QStringLiteral( "capture of %1" )
                     .arg( describeSubscriptions( config.subscriptions ) );
}

/// The capture of @p config reached its size limit and goes on in @p path:
/// open it in a new followed tab, and log why.  Not notified, as in
/// LogSquirl-Serial: a busy capture rotates often.
void openRotatedFile( const SessionConfig& config, const QString& path )
{
    openFollowed( path );
    hostLog( LOGSQUIRL_LOG_INFO,
             QStringLiteral( "NATS Monitor: the %1 reached its size limit of %2 MiB; "
                             "it goes on in %3." )
                 .arg( describeCapture( config ) )
                 .arg( config.maxFileBytes / kBytesPerMiB )
                 .arg( path ) );
}

} // namespace

Sessions::Sessions( QObject* parent )
    : QObject( parent )
{
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
    const auto fail = [ this, name ]( const QString& message ) {
        auto& status = entries_[ name ].status;
        status.state = SessionState::Failed;
        status.detail = message;
        Q_EMIT changed( name );
        return false;
    };
    if ( const auto problem = startProblem( profile ); !problem.isEmpty() ) {
        return fail( problem );
    }
    const auto config = sessionConfig( profile );
    if ( config.tls.skipVerify ) {
        hostLog( LOGSQUIRL_LOG_WARNING,
                 QStringLiteral( "NATS Monitor: the certificate of %1 is not verified (insecure)." )
                     .arg( redactUrls( config.url ) ) );
    }

    const auto subjects = describeCapture( config );
    auto* session = new NatsSession( config, this );
    // The session's signals come through the host's event loop: nothing
    // may throw into it.
    connect( session, &NatsSession::statusChanged, this, [ this, name ]( const QString& text ) {
        guarded( "showing a connection event", [ this, &name, &text ] {
            auto& status = entries_[ name ].status;
            status.detail = text;
            if ( text == QLatin1String( "disconnected" ) ) {
                status.state = SessionState::Connecting;
                status.server.clear();
            }
            Q_EMIT changed( name );
        } );
    } );
    connect( session, &NatsSession::connected, this, [ this, name ]( const QString& url ) {
        guarded( "showing a connection", [ this, &name, &url ] {
            auto& status = entries_[ name ].status;
            status.state = SessionState::Running;
            status.server = redactUrls( url );
            Q_EMIT changed( name );
        } );
    } );
    connect( session, &NatsSession::errorOccurred, this, [ subjects ]( const QString& message ) {
        guarded( "reporting a capture error", [ &subjects, &message ] {
            const auto text = QStringLiteral( "NATS Monitor (%1): %2" ).arg( subjects, message );
            hostLog( LOGSQUIRL_LOG_WARNING, text );
            hostNotify( text );
        } );
    } );
    // Ended on its own: its last event says why; the tab keeps what was
    // captured.
    connect( session, &NatsSession::finished, this, [ this, name ] {
        guarded( "ending a capture", [ this, &name ] { release( name, SessionState::Failed ); } );
    } );
    connect( session, &NatsSession::completed, this, [ this, name, subjects ] {
        guarded( "ending a replay", [ this, &name, &subjects ] {
            release( name, SessionState::Stopped, QStringLiteral( "replay complete" ) );
            hostNotify( QStringLiteral( "NATS %1 complete." ).arg( subjects ) );
        } );
    } );
    // In the session's context, not this one's: release() disconnects this
    // object, and a file the session goes on in while it stops still gets
    // its tab (the queued signal comes before the session's deleteLater()).
    connect( session, &NatsSession::fileRotated, session, [ session ]( const QString& path ) {
        guarded( "opening the next file",
                 [ session, &path ] { openRotatedFile( session->config(), path ); } );
    } );
    connect( session, &NatsSession::fileRotated, this, [ this, name ]( const QString& path ) {
        guarded( "showing the next file", [ this, &name, &path ] {
            auto& status = entries_[ name ].status;
            status.filePath = path;
            ++status.files;
            Q_EMIT changed( name );
        } );
    } );

    entries_[ name ].status.state = SessionState::Connecting;
    entries_[ name ].status.detail
        = QStringLiteral( "connecting to %1…" ).arg( redactUrls( config.url ) );
    if ( !session->start() ) {
        // start() has said why: statusChanged() set the detail.
        const auto detail = entries_[ name ].status.detail;
        delete session;
        return fail( detail );
    }
    auto& started = entries_[ name ];
    started.session = session;
    started.status.filePath = session->filePath();
    started.status.files = 1;
    Q_EMIT changed( name );

    openFollowed( session->filePath() );
    hostNotify(
        QStringLiteral( "NATS %1 started on %2." ).arg( subjects, redactUrls( config.url ) ) );
    return true;
}

void Sessions::stop( const QString& name, bool notify )
{
    const auto it = entries_.find( name );
    if ( it == entries_.end() || !it->session ) {
        return;
    }
    auto* session = it->session;
    const auto subjects = describeCapture( session->config() );
    session->stop();
    release( name, SessionState::Stopped, QStringLiteral( "stopped" ) );
    if ( notify ) {
        hostNotify( QStringLiteral( "NATS %1 stopped." ).arg( subjects ) );
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
    if ( it == entries_.constEnd() ) {
        return {};
    }
    auto status = it->status;
    if ( it->session ) {
        status.received = it->session->receivedCount();
        status.dropped = it->session->droppedCount();
    }
    return status;
}

void Sessions::release( const QString& name, SessionState state, const QString& detail )
{
    auto& entry = entries_[ name ];
    auto* session = entry.session;
    if ( !session ) {
        return; // released already
    }
    entry.session = nullptr;
    entry.status.received = session->receivedCount();
    entry.status.dropped = session->droppedCount();
    entry.status.state = state;
    entry.status.server.clear();
    if ( !detail.isEmpty() ) {
        entry.status.detail = detail;
    }
    session->preserveFile();
    session->disconnect( this );
    session->deleteLater();
    Q_EMIT changed( name );
}

} // namespace nats_monitor

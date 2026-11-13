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
 * @file sidebarwidget.cpp
 * @brief Implementation of the sidebar tab.
 */

#include "sidebarwidget.h"

#include "authmethods.h"
#include "connectiontile.h"
#include "natsoptions.h"
#include "plugin.h"
#include "profiles.h"
#include "replay.h"
#include "sessions.h"

#include <QFileInfo>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLocale>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

namespace nats_monitor {

namespace {

/// What @p profile is, for its tile.
QStringList summaryOf( const ConnectionProfile& profile )
{
    QString what;
    if ( profile.replay.enabled() ) {
        what = QStringLiteral( "Replays %1" ).arg( describeReplay( profile.replay ) );
    }
    else if ( const auto subjects = profile.subscriptions(); !subjects.isEmpty() ) {
        what = QStringLiteral( "Subjects  %1" ).arg( describeSubscriptions( subjects ) );
    }
    else {
        what = QStringLiteral( "no subject" );
    }
    return {
        redactUrls( profile.url ),
        QStringLiteral( "%1 · TLS %2" )
            .arg( QLatin1String( authMethodInfo( profile.auth.method ).label ),
                  profile.usesTls() ? QStringLiteral( "on" ) : QStringLiteral( "off" ) ),
        what,
    };
}

/// The capture's counts, and its file: its name, size and how many files
/// it wrote.
QStringList countsOf( const SessionStatus& status )
{
    const QLocale locale;
    QStringList lines{ QStringLiteral( "received %1 · dropped %2" )
                           .arg( locale.toString( status.received ),
                                 locale.toString( status.dropped ) ) };
    if ( !status.filePath.isEmpty() ) {
        const QFileInfo file( status.filePath );
        auto line
            = QStringLiteral( "%1 · %2" )
                  .arg( file.fileName(),
                        locale.formattedDataSize( file.size(), 1, QLocale::DataSizeIecFormat ) );
        if ( status.files > 1 ) {
            line += QStringLiteral( " · file %1" ).arg( status.files );
        }
        lines << line;
    }
    return lines;
}

} // namespace

TileView tileViewOf( const SessionStatus& status )
{
    TileView view;
    switch ( status.state ) {
    case SessionState::Stopped:
        view.state = TileState::Stopped;
        if ( !status.filePath.isEmpty() ) {
            // "stopped", or "replay complete"
            view.details << ( status.detail.isEmpty() ? QStringLiteral( "stopped" )
                                                      : status.detail )
                         << countsOf( status );
        }
        break;
    case SessionState::Connecting:
        view.state = TileState::Connecting;
        view.details << QStringLiteral( "connecting · %1" ).arg( status.detail )
                     << countsOf( status );
        break;
    case SessionState::Running:
        view.state = TileState::Running;
        view.details
            << QStringLiteral( "running · %1 msgs/s" ).arg( QLocale().toString( status.rate ) )
            << countsOf( status ) << QStringLiteral( "connected to %1" ).arg( status.server );
        break;
    case SessionState::Failed:
        view.state = TileState::Failed;
        view.details << QStringLiteral( "failed: %1" ).arg( status.detail );
        if ( !status.filePath.isEmpty() ) {
            view.details << countsOf( status );
        }
        break;
    }
    return view;
}

SidebarWidget::SidebarWidget( ProfileStore* profiles, Sessions* sessions, QWidget* parent )
    : QWidget( parent )
    , profiles_( profiles )
    , sessions_( sessions )
    , stopAll_( new QPushButton( QStringLiteral( "■ Stop all" ), this ) )
    , empty_( new QLabel( QStringLiteral( "No saved connection yet: New connection… adds one." ),
                          this ) )
    , tiles_( new QVBoxLayout )
{
    auto* newConnection = new QPushButton( QStringLiteral( "New connection…" ), this );
    newConnection->setObjectName( "newConnection" );
    newConnection->setToolTip(
        QStringLiteral( "Set up a NATS connection, the subjects it captures or the stream it "
                        "replays, and save it as a tile" ) );
    stopAll_->setObjectName( "stopAll" );
    stopAll_->setToolTip( QStringLiteral( "Stop every session; their tabs keep the files" ) );
    empty_->setObjectName( "empty" );
    empty_->setWordWrap( true );

    auto* buttons = new QHBoxLayout;
    buttons->addWidget( newConnection );
    buttons->addStretch();
    buttons->addWidget( stopAll_ );

    // The tiles scroll, the buttons stay.
    auto* content = new QWidget;
    auto* contentLayout = new QVBoxLayout( content );
    contentLayout->setContentsMargins( 0, 0, 0, 0 );
    contentLayout->setSpacing( 8 );
    tiles_->setSpacing( 8 );
    contentLayout->addWidget( empty_ );
    contentLayout->addLayout( tiles_ );
    contentLayout->addStretch();
    auto* scroll = new QScrollArea( this );
    scroll->setWidgetResizable( true );
    scroll->setFrameShape( QFrame::NoFrame );
    scroll->setHorizontalScrollBarPolicy( Qt::ScrollBarAlwaysOff );
    scroll->setWidget( content );

    auto* layout = new QVBoxLayout( this );
    layout->setContentsMargins( 4, 4, 4, 4 );
    layout->addLayout( buttons );
    layout->addWidget( scroll, 1 );

    connect( newConnection, &QPushButton::clicked, this, &SidebarWidget::newConnectionRequested );
    // Every slot runs in the host's event loop: nothing may throw into it.
    connect( stopAll_, &QPushButton::clicked, this, [ this ] {
        guarded( "stopping every session", [ this ] { sessions_->stopAll( true ); } );
    } );
    connect( sessions_, &Sessions::changed, this, [ this ]( const QString& name ) {
        guarded( "showing a session", [ this, &name ] {
            if ( auto* tile = tileByName_.value( name ) ) {
                updateTile( tile );
            }
            updateStopAll();
        } );
    } );
    connect( profiles_, &ProfileStore::changed, this,
             [ this ] { guarded( "showing the connections", [ this ] { reload(); } ); } );

    reload();
}

void SidebarWidget::reload()
{
    // Later: a tile's own click may have led here.
    for ( auto* tile : std::as_const( tileByName_ ) ) {
        tile->setParent( nullptr );
        tile->deleteLater();
    }
    tileByName_.clear();

    const auto profiles = profiles_->list();
    for ( const auto& profile : profiles ) {
        auto* tile = new ConnectionTile( profile.name, this );
        tile->setSummary( summaryOf( profile ) );
        const auto name = profile.name;
        connect( tile, &ConnectionTile::startClicked, this, [ this, name ] {
            guarded( "starting a session", [ this, &name ] { start( name ); } );
        } );
        connect( tile, &ConnectionTile::stopClicked, this, [ this, name ] {
            guarded( "stopping a session", [ this, &name ] { sessions_->stop( name ); } );
        } );
        connect( tile, &ConnectionTile::editClicked, this,
                 [ this, name ] { Q_EMIT editRequested( name ); } );
        connect( tile, &ConnectionTile::deleteClicked, this, [ this, name ] {
            guarded( "deleting a connection", [ this, &name ] { confirmDelete( name ); } );
        } );
        tiles_->addWidget( tile );
        tileByName_.insert( name, tile );
        updateTile( tile );
    }
    empty_->setVisible( profiles.isEmpty() );
    updateStopAll();
}

void SidebarWidget::start( const QString& name )
{
    auto profile = profiles_->find( name );
    if ( !profile ) {
        reload(); // deleted meanwhile
        return;
    }
    if ( const char* secret = authMethodInfo( profile->auth.method ).secret ) {
        bool ok = false;
        profile->auth.secret = QInputDialog::getText(
            this, QStringLiteral( "NATS Monitor" ),
            QStringLiteral( "The %1 for %2 (never saved):" ).arg( QLatin1String( secret ), name ),
            QLineEdit::Password, {}, &ok );
        if ( !ok || profile->auth.secret.isEmpty() ) {
            return;
        }
    }
    sessions_->start( *profile );
}

void SidebarWidget::confirmDelete( const QString& name )
{
    const auto answer
        = QMessageBox::question( this, QStringLiteral( "NATS Monitor" ),
                                 QStringLiteral( "Delete the saved connection %1?" ).arg( name ),
                                 QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel );
    if ( answer != QMessageBox::Yes ) {
        return;
    }
    // Deleted, the store's changed() reloads the tiles.
    if ( const auto problem = profiles_->remove( name ); !problem.isEmpty() ) {
        QMessageBox::warning( this, QStringLiteral( "NATS Monitor" ), problem );
        reload(); // e.g. deleted meanwhile
    }
}

void SidebarWidget::updateTile( ConnectionTile* tile )
{
    const auto view = tileViewOf( sessions_->status( tile->name() ) );
    tile->setState( view.state );
    tile->setDetails( view.details );
}

void SidebarWidget::updateStopAll()
{
    stopAll_->setEnabled( sessions_->anyRunning() );
}

} // namespace nats_monitor

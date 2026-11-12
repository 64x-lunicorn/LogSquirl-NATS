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
 * @file connectiontile.cpp
 * @brief Implementation of the connection tile.
 */

#include "connectiontile.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace nats_monitor {

namespace {

/// A label that wraps, and lets the text be selected and copied.
QLabel* textLabel( const char* name, QWidget* parent )
{
    auto* label = new QLabel( parent );
    label->setObjectName( name );
    label->setWordWrap( true );
    label->setTextFormat( Qt::PlainText );
    label->setTextInteractionFlags( Qt::TextSelectableByMouse );
    return label;
}

} // namespace

ConnectionTile::ConnectionTile( const QString& name, QWidget* parent )
    : QFrame( parent )
    , name_( name )
    , summary_( textLabel( "summary", this ) )
    , details_( textLabel( "details", this ) )
    , startStop_( new QPushButton( this ) )
    , remove_( new QPushButton( QStringLiteral( "Delete" ), this ) )
{
    setFrameShape( QFrame::StyledPanel );
    setObjectName( "tile" );

    auto* title = textLabel( "title", this );
    title->setText( name );
    auto font = title->font();
    font.setBold( true );
    title->setFont( font );

    // Secondary: smaller than the title and the summary.
    font = details_->font();
    font.setPointSizeF( font.pointSizeF() * 0.9 );
    details_->setFont( font );
    details_->hide();

    startStop_->setObjectName( "startStop" );
    auto* edit = new QPushButton( QStringLiteral( "Edit…" ), this );
    edit->setObjectName( "edit" );
    edit->setToolTip( QStringLiteral( "Change the settings of this connection" ) );
    remove_->setObjectName( "remove" );

    auto* head = new QHBoxLayout;
    head->addWidget( title, 1 );
    head->addWidget( startStop_ );

    auto* foot = new QHBoxLayout;
    foot->addStretch();
    foot->addWidget( edit );
    foot->addWidget( remove_ );

    auto* layout = new QVBoxLayout( this );
    layout->setContentsMargins( 6, 6, 6, 6 );
    layout->setSpacing( 4 );
    layout->addLayout( head );
    layout->addWidget( summary_ );
    layout->addWidget( details_ );
    layout->addLayout( foot );

    connect( startStop_, &QPushButton::clicked, this, [ this ] {
        if ( running_ ) {
            Q_EMIT stopClicked();
        }
        else {
            Q_EMIT startClicked();
        }
    } );
    connect( edit, &QPushButton::clicked, this, &ConnectionTile::editClicked );
    connect( remove_, &QPushButton::clicked, this, &ConnectionTile::deleteClicked );

    setRunning( false );
}

void ConnectionTile::setSummary( const QStringList& lines )
{
    summary_->setText( lines.join( QLatin1Char( '\n' ) ) );
}

void ConnectionTile::setDetails( const QStringList& lines )
{
    details_->setText( lines.join( QLatin1Char( '\n' ) ) );
    details_->setVisible( !lines.isEmpty() );
}

void ConnectionTile::setRunning( bool running )
{
    running_ = running;
    startStop_->setText( running ? QStringLiteral( "■ Stop" ) : QStringLiteral( "▶ Start" ) );
    startStop_->setToolTip( running ? QStringLiteral( "Stop the capture; its tab keeps the file" )
                                    : QStringLiteral( "Connect, and capture into a new tab" ) );
    remove_->setEnabled( !running );
    remove_->setToolTip( running ? QStringLiteral( "Stop the capture to delete the connection" )
                                 : QStringLiteral( "Delete this saved connection" ) );
}

} // namespace nats_monitor

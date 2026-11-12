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

#include <QApplication>
#include <QColor>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QStyle>
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

/// @p from mixed with @p to: 0 is @p from, 1 is @p to.
QColor mix( const QColor& from, const QColor& to, double amount )
{
    const auto channel
        = [ amount ]( int a, int b ) { return static_cast<int>( a + ( b - a ) * amount + 0.5 ); };
    return QColor( channel( from.red(), to.red() ), channel( from.green(), to.green() ),
                   channel( from.blue(), to.blue() ) );
}

/// The state as the dot's property and the stylesheet name it.
const char* stateName( TileState state )
{
    switch ( state ) {
    case TileState::Connecting:
        return "connecting";
    case TileState::Running:
        return "running";
    case TileState::Failed:
        return "failed";
    case TileState::Stopped:
        break;
    }
    return "stopped";
}

/// Have @p widget's style see a changed dynamic property.
void repolish( QWidget* widget )
{
    widget->style()->unpolish( widget );
    widget->style()->polish( widget );
}

/// The dot's diameter, in pixels.
constexpr int kDot = 10;

} // namespace

ConnectionTile::ConnectionTile( const QString& name, QWidget* parent )
    : QFrame( parent )
    , name_( name )
    , state_( new QLabel( this ) )
    , summary_( textLabel( "summary", this ) )
    , separator_( new QFrame( this ) )
    , details_( textLabel( "details", this ) )
    , startStop_( new QPushButton( this ) )
    , delete_( new QPushButton( QStringLiteral( "Delete" ), this ) )
{
    setObjectName( "tile" );

    state_->setObjectName( "state" );
    state_->setFixedSize( kDot, kDot );

    auto* title = textLabel( "title", this );
    title->setText( name );
    auto font = title->font();
    font.setBold( true );
    title->setFont( font );

    separator_->setObjectName( "separator" );
    separator_->setFixedHeight( 1 );
    separator_->hide();

    // Secondary: smaller than the title and the summary, and muted.
    font = details_->font();
    font.setPointSizeF( font.pointSizeF() * 0.9 );
    details_->setFont( font );
    details_->hide();

    startStop_->setObjectName( "startStop" );
    auto* edit = new QPushButton( QStringLiteral( "Edit…" ), this );
    edit->setObjectName( "edit" );
    edit->setToolTip( QStringLiteral( "Change the settings of this connection" ) );
    delete_->setObjectName( "delete" );
    delete_->setToolTip( QStringLiteral( "Delete this saved connection" ) );

    auto* head = new QHBoxLayout;
    head->setSpacing( 8 );
    head->addWidget( state_, 0, Qt::AlignVCenter );
    head->addWidget( title, 1 );
    head->addWidget( startStop_ );

    auto* foot = new QHBoxLayout;
    foot->addStretch();
    foot->addWidget( edit );
    foot->addWidget( delete_ );

    auto* layout = new QVBoxLayout( this );
    layout->setContentsMargins( 12, 10, 12, 10 );
    layout->setSpacing( 6 );
    layout->addLayout( head );
    layout->addWidget( summary_ );
    layout->addWidget( separator_ );
    layout->addWidget( details_ );
    layout->addLayout( foot );

    connect( startStop_, &QPushButton::clicked, this, [ this ] {
        if ( active_ ) {
            Q_EMIT stopClicked();
        }
        else {
            Q_EMIT startClicked();
        }
    } );
    connect( edit, &QPushButton::clicked, this, &ConnectionTile::editClicked );
    connect( delete_, &QPushButton::clicked, this, &ConnectionTile::deleteClicked );

    setState( TileState::Stopped );
    watched_ = parentWidget();
    if ( watched_ ) {
        watched_->installEventFilter( this );
    }
    applyStyle();
}

void ConnectionTile::setSummary( const QStringList& lines )
{
    summary_->setText( lines.join( QLatin1Char( '\n' ) ) );
}

void ConnectionTile::setDetails( const QStringList& lines )
{
    details_->setText( lines.join( QLatin1Char( '\n' ) ) );
    details_->setVisible( !lines.isEmpty() );
    separator_->setVisible( !lines.isEmpty() );
}

void ConnectionTile::setState( TileState state )
{
    active_ = state == TileState::Connecting || state == TileState::Running;
    startStop_->setText( active_ ? QStringLiteral( "■ Stop" ) : QStringLiteral( "▶ Start" ) );
    startStop_->setToolTip( active_ ? QStringLiteral( "Stop the capture; its tab keeps the file" )
                                    : QStringLiteral( "Connect, and capture into a new tab" ) );
    delete_->setVisible( !active_ );

    const auto* name = stateName( state );
    state_->setProperty( "state", QString::fromLatin1( name ) );
    state_->setToolTip( QString::fromLatin1( name ) );
    setProperty( "running", active_ );
    repolish( state_ );
    repolish( this );
}

void ConnectionTile::changeEvent( QEvent* event )
{
    QFrame::changeEvent( event );
    switch ( event->type() ) {
    case QEvent::ParentChange:
        // Its stylesheet keeps a palette change of the parent from reaching
        // the tile: it watches the parent itself.
        if ( watched_ ) {
            watched_->removeEventFilter( this );
        }
        watched_ = parentWidget();
        if ( watched_ ) {
            watched_->installEventFilter( this );
        }
        applyStyle();
        break;
    case QEvent::PaletteChange:
    case QEvent::ApplicationPaletteChange:
        applyStyle();
        break;
    default:
        break;
    }
}

bool ConnectionTile::eventFilter( QObject* watched, QEvent* event )
{
    if ( watched == watched_ && event->type() == QEvent::PaletteChange ) {
        applyStyle();
    }
    return QFrame::eventFilter( watched, event );
}

void ConnectionTile::applyStyle()
{
    // From the palette around the tile: its own is changed by its
    // stylesheet, and styling from it would drift a little further at each
    // palette change.
    const auto source = parentWidget() ? parentWidget()->palette() : QApplication::palette( this );
    const auto window = source.color( QPalette::Window );
    const auto text = source.color( QPalette::WindowText );
    const auto accent = source.color( QPalette::Highlight );
    const auto styledFor = QStringList{ window.name(), text.name(), accent.name() }.join( ' ' );
    if ( styledFor == styledFor_ ) {
        return;
    }
    styledFor_ = styledFor;
    const bool dark = window.lightness() < 128;
    const auto card = dark ? window.lighter( 125 ) : window.darker( 103 );
    const auto border = mix( window, text, 0.22 );
    const auto muted = mix( window, text, 0.62 );
    const auto stopped = mix( window, text, 0.4 );
    setStyleSheet( QStringLiteral( "QFrame#tile { background: %1; border: 1px solid %2;"
                                   " border-radius: 8px; }"
                                   "QFrame#tile[running=\"true\"] { border-color: %3; }"
                                   "QFrame#separator { background: %2; border: none; }"
                                   "QLabel#title, QLabel#summary, QLabel#details"
                                   " { background: transparent; border: none; color: %7; }"
                                   "QLabel#details { color: %4; }"
                                   "QLabel#state { border: none; border-radius: %5px;"
                                   " background: %6; }"
                                   "QLabel#state[state=\"connecting\"] { background: #d29922; }"
                                   "QLabel#state[state=\"running\"] { background: #2ea043; }"
                                   "QLabel#state[state=\"failed\"] { background: #e5534b; }" )
                       .arg( card.name(), border.name(), accent.name(), muted.name() )
                       .arg( kDot / 2 )
                       .arg( stopped.name(), text.name() ) );
}

} // namespace nats_monitor

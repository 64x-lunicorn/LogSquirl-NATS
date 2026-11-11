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
 * @file statuswidget.cpp
 * @brief Implementation of the status bar widget.
 */

#include "statuswidget.h"

#include <QLocale>
#include <QTimer>

#include <algorithm>

namespace nats_monitor {

namespace {

/// How often the rate is sampled.
constexpr int kSampleMs = 1000;

} // namespace

StatusWidget::StatusWidget( const ConnectionDialog* dialog, QWidget* parent )
    : QLabel( parent )
    , dialog_( dialog )
    , timer_( new QTimer( this ) )
{
    setObjectName( "natsStatus" );
    setToolTip( QStringLiteral( "NATS Monitor: the running captures, the messages they receive "
                                "a second, and how many they dropped (each file marks where)" ) );
    receivedAtSample_ = dialog_->totals().received;
    sinceSample_.start();
    connect( timer_, &QTimer::timeout, this, [ this ] { refresh( true ); } );
    connect( dialog_, &ConnectionDialog::sessionsChanged, this, [ this ] { refresh( false ); } );
    timer_->start( kSampleMs );
    refresh( false );
}

void StatusWidget::refresh( bool sample )
{
    const auto totals = dialog_->totals();
    if ( sample ) {
        const auto elapsedMs = std::max<qint64>( sinceSample_.restart(), 1 );
        rate_ = ( totals.received - receivedAtSample_ ) * 1000 / static_cast<quint64>( elapsedMs );
        receivedAtSample_ = totals.received;
    }
    if ( totals.sessions == 0 ) {
        rate_ = 0;
        setText( QStringLiteral( "NATS: no capture" ) );
        return;
    }
    const QLocale locale;
    setText( QStringLiteral( "NATS: %1 %2 · %3 msgs/s · %4 dropped" )
                 .arg( totals.sessions )
                 .arg( totals.sessions == 1 ? QStringLiteral( "session" )
                                            : QStringLiteral( "sessions" ) )
                 .arg( locale.toString( rate_ ), locale.toString( totals.dropped ) ) );
}

} // namespace nats_monitor

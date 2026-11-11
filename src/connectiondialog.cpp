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
 * @file connectiondialog.cpp
 * @brief Implementation of the NATS Monitor dialog.
 */

#include "connectiondialog.h"

#include "natssession.h"
#include "plugin.h"

#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

namespace nats_monitor {

ConnectionDialog::ConnectionDialog( QWidget* parent )
    : QDialog( parent )
    , urlEdit_( new QLineEdit( QString::fromLatin1( kDefaultUrl ), this ) )
    , subjectEdit_( new QLineEdit( this ) )
    , startButton_( new QPushButton( QStringLiteral( "▶ Start" ), this ) )
    , stopButton_( new QPushButton( QStringLiteral( "■ Stop" ), this ) )
    , statusLabel_( new QLabel( QStringLiteral( "Not capturing." ), this ) )
{
    setWindowTitle( QStringLiteral( "NATS Monitor" ) );

    urlEdit_->setObjectName( "url" );
    urlEdit_->setToolTip( QStringLiteral( "The server, e.g. nats://localhost:4222; several "
                                          "servers of one cluster comma-separated" ) );
    subjectEdit_->setObjectName( "subject" );
    subjectEdit_->setPlaceholderText( QStringLiteral( "orders.>" ) );
    subjectEdit_->setToolTip( QStringLiteral( "The subject to capture: * matches one token, "
                                              "> the rest of the subject" ) );
    startButton_->setObjectName( "start" );
    startButton_->setDefault( true );
    stopButton_->setObjectName( "stop" );
    statusLabel_->setObjectName( "status" );
    statusLabel_->setWordWrap( true );
    statusLabel_->setTextInteractionFlags( Qt::TextSelectableByMouse );

    auto* form = new QFormLayout;
    form->addRow( QStringLiteral( "Server:" ), urlEdit_ );
    form->addRow( QStringLiteral( "Subject:" ), subjectEdit_ );

    auto* buttons = new QHBoxLayout;
    buttons->addStretch();
    buttons->addWidget( startButton_ );
    buttons->addWidget( stopButton_ );

    auto* layout = new QVBoxLayout( this );
    layout->addLayout( form );
    layout->addLayout( buttons );
    layout->addWidget( statusLabel_ );

    connect( startButton_, &QPushButton::clicked, this, &ConnectionDialog::startCapture );
    connect( stopButton_, &QPushButton::clicked, this, &ConnectionDialog::stopCapture );
    connect( subjectEdit_, &QLineEdit::returnPressed, this, [ this ] {
        if ( !session_ ) {
            startCapture();
        }
    } );

    updateButtons();
    resize( 480, sizeHint().height() );
}

ConnectionDialog::~ConnectionDialog()
{
    stopAll();
}

void ConnectionDialog::stopAll()
{
    if ( !session_ ) {
        return;
    }
    session_->stop();
    releaseSession();
    statusLabel_->setText( QStringLiteral( "Stopped." ) );
}

bool ConnectionDialog::isCapturing() const
{
    return session_ != nullptr;
}

void ConnectionDialog::startCapture()
{
    if ( session_ ) {
        return;
    }

    SessionConfig config;
    config.url = urlEdit_->text().trimmed();
    config.subject = subjectEdit_->text().trimmed();
    if ( config.url.isEmpty() ) {
        statusLabel_->setText( QStringLiteral( "Enter the server URL." ) );
        return;
    }
    if ( config.subject.isEmpty() ) {
        statusLabel_->setText( QStringLiteral( "Enter the subject to capture." ) );
        return;
    }

    auto* session = new NatsSession( config, this );
    connect( session, &NatsSession::statusChanged, statusLabel_, &QLabel::setText );
    connect( session, &NatsSession::errorOccurred, this,
             [ subject = config.subject ]( const QString& message ) {
                 hostLog( LOGSQUIRL_LOG_WARNING,
                          QStringLiteral( "NATS Monitor (%1): %2" ).arg( subject, message ) );
             } );
    connect( session, &NatsSession::finished, this, &ConnectionDialog::onSessionFinished );

    statusLabel_->setText( QStringLiteral( "Connecting to %1…" ).arg( config.url ) );
    if ( !session->start() ) {
        // start() has said why, in the status line and the log
        delete session;
        return;
    }
    session_ = session;
    updateButtons();

    if ( g_state.api && g_state.handle ) {
        g_state.api->open_file( g_state.handle, session->filePath().toUtf8().constData(), 1 );
    }
}

void ConnectionDialog::stopCapture()
{
    stopAll();
}

void ConnectionDialog::onSessionFinished()
{
    // The status line shows why; the tab keeps what was captured.
    releaseSession();
}

void ConnectionDialog::releaseSession()
{
    session_->preserveFile();
    session_->disconnect( this );
    session_->disconnect( statusLabel_ );
    session_->deleteLater();
    session_ = nullptr;
    updateButtons();
}

void ConnectionDialog::updateButtons()
{
    const bool capturing = session_ != nullptr;
    startButton_->setEnabled( !capturing );
    stopButton_->setEnabled( capturing );
    urlEdit_->setEnabled( !capturing );
    subjectEdit_->setEnabled( !capturing );
}

} // namespace nats_monitor

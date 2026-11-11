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

#include "authmethods.h"
#include "natsoptions.h"
#include "natssession.h"
#include "plugin.h"
#include "profiles.h"
#include "subjects.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <limits>

namespace nats_monitor {

namespace {

/// nats.c takes the byte limit as an int.
constexpr int kMaxPendingMegabytes
    = static_cast<int>( std::numeric_limits<int>::max() / kBytesPerMiB );

/// The largest size limit the dialog takes: 1 TiB.
constexpr int kMaxFileMiB = 1024 * 1024;

/// Where the size limit is saved.
constexpr const char* kMaxFileMiBKey = "capture/maxFileMiB";

/// The plugin's settings file, nats.ini in its configuration directory;
/// empty without a host to say where that is.
QString settingsFile()
{
    if ( !g_state.api || !g_state.handle || !g_state.api->get_config_dir ) {
        return {};
    }
    const char* dir = g_state.api->get_config_dir( g_state.handle );
    if ( !dir || !*dir ) {
        return {};
    }
    return QString::fromUtf8( dir ) + QStringLiteral( "/nats.ini" );
}

/// The size limit saved last time, or the default.
int savedMaxFileMiB()
{
    const auto file = settingsFile();
    if ( file.isEmpty() ) {
        return kDefaultMaxFileMiB;
    }
    bool ok = false;
    const auto value = QSettings( file, QSettings::IniFormat )
                           .value( kMaxFileMiBKey, kDefaultMaxFileMiB )
                           .toInt( &ok );
    return ok && value >= 0 && value <= kMaxFileMiB ? value : kDefaultMaxFileMiB;
}

/// Save the size limit @p mib for the next session.
void saveMaxFileMiB( int mib )
{
    const auto file = settingsFile();
    if ( file.isEmpty() ) {
        return;
    }
    QSettings settings( file, QSettings::IniFormat );
    settings.setValue( kMaxFileMiBKey, mib );
    settings.sync();
    if ( settings.status() != QSettings::NoError ) {
        hostLog( LOGSQUIRL_LOG_WARNING,
                 QStringLiteral( "NATS Monitor: the size limit could not be saved to %1." )
                     .arg( file ) );
    }
}

/// The capture of @p config reached its size limit and goes on in @p path:
/// open it in a new followed tab, and log why.  Not notified, as in
/// LogSquirl-Serial: a busy capture rotates often.
void openRotatedFile( const SessionConfig& config, const QString& path )
{
    if ( g_state.api && g_state.handle ) {
        g_state.api->open_file( g_state.handle, path.toUtf8().constData(), 1 );
    }
    hostLog( LOGSQUIRL_LOG_INFO,
             QStringLiteral( "NATS Monitor: the capture of %1 reached its size limit of %2 MiB; "
                             "it goes on in %3." )
                 .arg( describeSubscriptions( config.subscriptions ) )
                 .arg( config.maxFileBytes / kBytesPerMiB )
                 .arg( path ) );
}

/// The method chosen in @p combo.
const AuthMethodInfo& chosenMethod( const QComboBox* combo )
{
    return authMethodInfo( static_cast<AuthMethod>( combo->currentData().toInt() ) );
}

} // namespace

ConnectionDialog::ConnectionDialog( QWidget* parent )
    : QDialog( parent )
    , profileCombo_( new QComboBox( this ) )
    , saveProfileButton_( new QPushButton( QStringLiteral( "Save" ), this ) )
    , deleteProfileButton_( new QPushButton( QStringLiteral( "Delete" ), this ) )
    , urlEdit_( new QLineEdit( QString::fromLatin1( kDefaultUrl ), this ) )
    , authCombo_( new QComboBox( this ) )
    , userEdit_( new QLineEdit( this ) )
    , secretEdit_( new QLineEdit( this ) )
    , authFileEdit_( new QLineEdit( this ) )
    , authFileButton_( new QPushButton( QStringLiteral( "…" ), this ) )
    , tlsCheck_( new QCheckBox( QStringLiteral( "Use TLS" ), this ) )
    , caFileEdit_( new QLineEdit( this ) )
    , certFileEdit_( new QLineEdit( this ) )
    , keyFileEdit_( new QLineEdit( this ) )
    , serverNameEdit_( new QLineEdit( this ) )
    , skipVerifyCheck_( new QCheckBox(
          QStringLiteral( "Skip the verification of the server's certificate (insecure)" ), this ) )
    , subjectsEdit_( new QPlainTextEdit( this ) )
    , subjectsWarning_( new QLabel( this ) )
    , maxBinarySpin_( new QSpinBox( this ) )
    , pendingMessagesSpin_( new QSpinBox( this ) )
    , pendingMegabytesSpin_( new QSpinBox( this ) )
    , maxFileSpin_( new QSpinBox( this ) )
    , startButton_( new QPushButton( QStringLiteral( "▶ Start" ), this ) )
    , sessionList_( new QListWidget( this ) )
    , stopButton_( new QPushButton( QStringLiteral( "■ Stop" ), this ) )
    , stopAllButton_( new QPushButton( QStringLiteral( "Stop all" ), this ) )
    , statusLabel_( new QLabel( QStringLiteral( "Not capturing." ), this ) )
{
    setWindowTitle( QStringLiteral( "NATS Monitor" ) );

    profileCombo_->setObjectName( "profile" );
    profileCombo_->setEditable( true );
    profileCombo_->setInsertPolicy( QComboBox::NoInsert );
    profileCombo_->lineEdit()->setPlaceholderText( QStringLiteral( "Name of a saved connection" ) );
    profileCombo_->setToolTip(
        QStringLiteral( "Choose a saved connection, or enter a name and Save the settings below "
                        "under it; passwords and tokens are never saved" ) );
    saveProfileButton_->setObjectName( "saveProfile" );
    saveProfileButton_->setAutoDefault( false );
    deleteProfileButton_->setObjectName( "deleteProfile" );
    deleteProfileButton_->setAutoDefault( false );

    urlEdit_->setObjectName( "url" );
    urlEdit_->setToolTip( QStringLiteral( "The server, e.g. nats://localhost:4222; several "
                                          "servers of one cluster comma-separated" ) );

    authCombo_->setObjectName( "authMethod" );
    for ( const auto& info : kAuthMethods ) {
        authCombo_->addItem( QString::fromLatin1( info.label ), static_cast<int>( info.method ) );
    }
    userEdit_->setObjectName( "user" );
    secretEdit_->setObjectName( "secret" );
    secretEdit_->setEchoMode( QLineEdit::Password );
    secretEdit_->setToolTip( QStringLiteral( "Never saved: asked for at Start" ) );
    authFileEdit_->setObjectName( "authFile" );
    authFileButton_->setAutoDefault( false );

    tlsCheck_->setObjectName( "tls" );
    tlsCheck_->setToolTip( QStringLiteral(
        "Connect with TLS; a tls:// URL does so too. The settings below apply either way" ) );
    caFileEdit_->setObjectName( "caFile" );
    caFileEdit_->setPlaceholderText( QStringLiteral( "PEM file of the CA that signed the "
                                                     "server's certificate" ) );
    certFileEdit_->setObjectName( "certFile" );
    certFileEdit_->setPlaceholderText( QStringLiteral( "Only if the server asks for one" ) );
    keyFileEdit_->setObjectName( "keyFile" );
    serverNameEdit_->setObjectName( "serverName" );
    serverNameEdit_->setPlaceholderText( QStringLiteral( "The URL's host" ) );
    serverNameEdit_->setToolTip(
        QStringLiteral( "The name the server's certificate must be for, if not the URL's host" ) );
    skipVerifyCheck_->setObjectName( "skipVerify" );
    skipVerifyCheck_->setToolTip(
        QStringLiteral( "Accepts any certificate, so anyone between LogSquirl and the server can "
                        "read and change the traffic: for testing only" ) );
    subjectsEdit_->setObjectName( "subjects" );
    subjectsEdit_->setTabChangesFocus( true );
    subjectsEdit_->setPlaceholderText( QStringLiteral( "orders.>\njobs.* workers" ) );
    subjectsEdit_->setToolTip(
        QStringLiteral( "The subjects to capture into one file, one a line: * matches one "
                        "token, > the rest of the subject. A queue group after a subject "
                        "subscribes in it: the group's members then share the messages, so "
                        "the capture takes them away from the others" ) );
    subjectsEdit_->setFixedHeight(
        subjectsEdit_->fontMetrics().lineSpacing() * 4 + 2 * subjectsEdit_->frameWidth()
        + static_cast<int>( subjectsEdit_->document()->documentMargin() * 2 ) );
    subjectsWarning_->setObjectName( "subjectsWarning" );
    subjectsWarning_->setWordWrap( true );
    subjectsWarning_->hide();
    maxBinarySpin_->setObjectName( "maxBinaryBytes" );
    maxBinarySpin_->setRange( 0, 1024 * 1024 );
    maxBinarySpin_->setSuffix( QStringLiteral( " bytes" ) );
    maxBinarySpin_->setValue( kDefaultMaxBinaryBytes );
    maxBinarySpin_->setToolTip(
        QStringLiteral( "How much of a payload that is not UTF-8 text is "
                        "written, as hex; its size is always the real one" ) );
    pendingMessagesSpin_->setObjectName( "pendingMessages" );
    pendingMessagesSpin_->setRange( 1, std::numeric_limits<int>::max() );
    pendingMessagesSpin_->setGroupSeparatorShown( true );
    pendingMessagesSpin_->setSuffix( QStringLiteral( " messages" ) );
    pendingMessagesSpin_->setValue( kDefaultPendingLimits.messages );
    pendingMegabytesSpin_->setObjectName( "pendingMegabytes" );
    pendingMegabytesSpin_->setRange( 1, kMaxPendingMegabytes );
    pendingMegabytesSpin_->setSuffix( QStringLiteral( " MiB" ) );
    pendingMegabytesSpin_->setValue(
        static_cast<int>( kDefaultPendingLimits.bytes / kBytesPerMiB ) );
    for ( auto* spin : { pendingMessagesSpin_, pendingMegabytesSpin_ } ) {
        spin->setToolTip( QStringLiteral(
            "How much the NATS client holds for the capture while it writes: beyond it, "
            "messages are dropped as a slow consumer, and the file says how many" ) );
    }
    maxFileSpin_->setObjectName( "maxFileMiB" );
    maxFileSpin_->setRange( 0, kMaxFileMiB );
    maxFileSpin_->setGroupSeparatorShown( true );
    maxFileSpin_->setSuffix( QStringLiteral( " MiB" ) );
    maxFileSpin_->setSpecialValueText( QStringLiteral( "No limit" ) );
    maxFileSpin_->setValue( savedMaxFileMiB() );
    maxFileSpin_->setToolTip(
        QStringLiteral( "When a file reaches this size, the capture goes on in a new file, "
                        "opened in a new tab; remembered for the next session" ) );
    startButton_->setObjectName( "start" );
    startButton_->setDefault( true );
    sessionList_->setObjectName( "sessions" );
    sessionList_->setToolTip(
        QStringLiteral( "The running captures; Stop stops the one selected" ) );
    sessionList_->setFixedHeight( sessionList_->fontMetrics().lineSpacing() * 4
                                  + 2 * sessionList_->frameWidth() );
    stopButton_->setObjectName( "stop" );
    stopButton_->setAutoDefault( false );
    stopAllButton_->setObjectName( "stopAll" );
    stopAllButton_->setAutoDefault( false );
    statusLabel_->setObjectName( "status" );
    statusLabel_->setWordWrap( true );
    statusLabel_->setTextInteractionFlags( Qt::TextSelectableByMouse );

    // A file field with its "…" button, which chooses the file.
    const auto fileRow = [ this ]( QLineEdit* edit, QPushButton* chooser, const QString& title ) {
        chooser->setAutoDefault( false );
        connect( chooser, &QPushButton::clicked, this,
                 [ this, edit, title ] { browse( edit, title ); } );
        auto* row = new QHBoxLayout;
        row->addWidget( edit );
        row->addWidget( chooser );
        return row;
    };

    auto* profileRow = new QHBoxLayout;
    profileRow->addWidget( profileCombo_, 1 );
    profileRow->addWidget( saveProfileButton_ );
    profileRow->addWidget( deleteProfileButton_ );

    auto* auth = new QFormLayout;
    auth->addRow( QStringLiteral( "Method:" ), authCombo_ );
    auth->addRow( QStringLiteral( "User:" ), userEdit_ );
    auth->addRow( QStringLiteral( "Password / token:" ), secretEdit_ );
    auth->addRow( QStringLiteral( "File:" ),
                  fileRow( authFileEdit_, authFileButton_,
                           QStringLiteral( "Credentials or NKey seed file" ) ) );
    auto* authBox = new QGroupBox( QStringLiteral( "Authentication" ), this );
    authBox->setLayout( auth );

    auto* tls = new QFormLayout;
    tls->addRow( tlsCheck_ );
    const auto tlsFile = [ &, this ]( const QString& title, QLineEdit* edit ) {
        auto* label = new QLabel( title + QLatin1Char( ':' ), this );
        auto* chooser = new QPushButton( QStringLiteral( "…" ), this );
        tls->addRow( label, fileRow( edit, chooser, title ) );
        tlsWidgets_ << label << edit << chooser;
    };
    tlsFile( QStringLiteral( "CA file" ), caFileEdit_ );
    tlsFile( QStringLiteral( "Client certificate" ), certFileEdit_ );
    tlsFile( QStringLiteral( "Client key" ), keyFileEdit_ );
    auto* serverNameLabel = new QLabel( QStringLiteral( "Server name:" ), this );
    tls->addRow( serverNameLabel, serverNameEdit_ );
    tls->addRow( skipVerifyCheck_ );
    tlsWidgets_ << serverNameLabel << serverNameEdit_ << skipVerifyCheck_;
    tlsTexts_ = { { caFileEdit_, &TlsConfig::caFile },
                  { certFileEdit_, &TlsConfig::certFile },
                  { keyFileEdit_, &TlsConfig::keyFile },
                  { serverNameEdit_, &TlsConfig::serverName } };
    auto* tlsBox = new QGroupBox( QStringLiteral( "TLS" ), this );
    tlsBox->setLayout( tls );

    auto* connection = new QFormLayout;
    connection->addRow( QStringLiteral( "Profile:" ), profileRow );
    connection->addRow( QStringLiteral( "Server:" ), urlEdit_ );

    auto* form = new QFormLayout;
    form->addRow( QStringLiteral( "Subjects:" ), subjectsEdit_ );
    form->addRow( QString(), subjectsWarning_ );
    form->addRow( QStringLiteral( "Binary payloads, first:" ), maxBinarySpin_ );
    auto* pending = new QHBoxLayout;
    pending->addWidget( pendingMessagesSpin_ );
    pending->addWidget( pendingMegabytesSpin_ );
    form->addRow( QStringLiteral( "Pending at most:" ), pending );
    form->addRow( QStringLiteral( "New file at:" ), maxFileSpin_ );

    auto* buttons = new QHBoxLayout;
    buttons->addStretch();
    buttons->addWidget( startButton_ );

    auto* sessionButtons = new QHBoxLayout;
    sessionButtons->addStretch();
    sessionButtons->addWidget( stopButton_ );
    sessionButtons->addWidget( stopAllButton_ );

    auto* layout = new QVBoxLayout( this );
    layout->addLayout( connection );
    layout->addWidget( authBox );
    layout->addWidget( tlsBox );
    layout->addLayout( form );
    layout->addLayout( buttons );
    layout->addWidget( new QLabel( QStringLiteral( "Running:" ), this ) );
    layout->addWidget( sessionList_ );
    layout->addLayout( sessionButtons );
    layout->addWidget( statusLabel_ );

    connect( startButton_, &QPushButton::clicked, this, &ConnectionDialog::startCapture );
    connect( stopButton_, &QPushButton::clicked, this, &ConnectionDialog::stopSelected );
    connect( stopAllButton_, &QPushButton::clicked, this, &ConnectionDialog::stopAllCaptures );
    connect( sessionList_, &QListWidget::currentRowChanged, this,
             &ConnectionDialog::updateButtons );
    connect( subjectsEdit_, &QPlainTextEdit::textChanged, this,
             &ConnectionDialog::updateSubjectsWarning );
    connect( profileCombo_, &QComboBox::currentIndexChanged, this, &ConnectionDialog::loadProfile );
    connect( saveProfileButton_, &QPushButton::clicked, this, &ConnectionDialog::saveProfile );
    connect( deleteProfileButton_, &QPushButton::clicked, this, &ConnectionDialog::deleteProfile );
    connect( authCombo_, &QComboBox::currentIndexChanged, this, &ConnectionDialog::updateButtons );
    connect( tlsCheck_, &QCheckBox::toggled, this, &ConnectionDialog::updateButtons );
    connect( urlEdit_, &QLineEdit::textChanged, this, &ConnectionDialog::updateButtons );

    fillProfileList( {} );
    updateButtons();
    resize( 520, sizeHint().height() );
}

ConnectionProfile ConnectionDialog::currentProfile() const
{
    ConnectionProfile profile;
    profile.name = profileCombo_->currentText().trimmed();
    profile.url = urlEdit_->text().trimmed();
    const auto& method = chosenMethod( authCombo_ );
    profile.auth.method = method.method;
    if ( method.takesUser ) {
        profile.auth.user = userEdit_->text().trimmed();
    }
    if ( method.secret ) {
        profile.auth.secret = secretEdit_->text();
    }
    if ( method.file ) {
        profile.auth.file = authFileEdit_->text().trimmed();
    }
    profile.tls.enabled = tlsCheck_->isChecked();
    for ( const auto& [ edit, member ] : tlsTexts_ ) {
        profile.tls.*member = edit->text().trimmed();
    }
    profile.tls.skipVerify = skipVerifyCheck_->isChecked();
    return profile;
}

void ConnectionDialog::fillProfileList( const QString& current )
{
    fillingProfiles_ = true;
    profileCombo_->clear();
    for ( const auto& profile : loadProfiles( settingsFile() ) ) {
        profileCombo_->addItem( profile.name );
    }
    profileCombo_->setCurrentIndex( profileCombo_->findText( current ) );
    profileCombo_->setEditText( current );
    fillingProfiles_ = false;
}

void ConnectionDialog::loadProfile( int index )
{
    if ( fillingProfiles_ || index < 0 ) {
        return;
    }
    const auto name = profileCombo_->itemText( index );
    for ( const auto& profile : loadProfiles( settingsFile() ) ) {
        if ( profile.name != name ) {
            continue;
        }
        urlEdit_->setText( profile.url );
        authCombo_->setCurrentIndex(
            authCombo_->findData( static_cast<int>( profile.auth.method ) ) );
        userEdit_->setText( profile.auth.user );
        secretEdit_->clear();
        authFileEdit_->setText( profile.auth.file );
        tlsCheck_->setChecked( profile.tls.enabled );
        for ( const auto& [ edit, member ] : tlsTexts_ ) {
            edit->setText( profile.tls.*member );
        }
        skipVerifyCheck_->setChecked( profile.tls.skipVerify );
        updateButtons();

        const char* secret = authMethodInfo( profile.auth.method ).secret;
        if ( !secret ) {
            statusLabel_->setText( QStringLiteral( "Profile %1 loaded." ).arg( name ) );
        }
        else {
            statusLabel_->setText(
                QStringLiteral( "Profile %1 loaded. Enter the %2 (it is never saved), then Start." )
                    .arg( name, QLatin1String( secret ) ) );
            secretEdit_->setFocus();
        }
        return;
    }
}

void ConnectionDialog::saveProfile()
{
    auto profile = currentProfile();
    if ( profile.name.isEmpty() ) {
        statusLabel_->setText( QStringLiteral( "Enter a name for the profile to save." ) );
        return;
    }
    for ( const auto& url : profile.url.split( ',' ) ) {
        if ( !QUrl( url.trimmed() ).userInfo().isEmpty() ) {
            statusLabel_->setText(
                QStringLiteral( "The URL holds credentials, which a profile never saves: enter "
                                "them under Authentication instead." ) );
            return;
        }
    }
    profile.auth.secret.clear(); // never saved
    auto profiles = loadProfiles( settingsFile() );
    const auto existing = std::find_if( profiles.begin(), profiles.end(),
                                        [ & ]( const auto& p ) { return p.name == profile.name; } );
    if ( existing != profiles.end() ) {
        *existing = profile;
    }
    else {
        profiles << profile;
    }
    if ( !saveProfiles( settingsFile(), profiles ) ) {
        statusLabel_->setText( QStringLiteral( "The profile %1 could not be saved to %2." )
                                   .arg( profile.name, settingsFile() ) );
        return;
    }
    fillProfileList( profile.name );
    const char* secret = authMethodInfo( profile.auth.method ).secret;
    statusLabel_->setText( secret ? QStringLiteral( "Profile %1 saved, without the %2." )
                                        .arg( profile.name, QLatin1String( secret ) )
                                  : QStringLiteral( "Profile %1 saved." ).arg( profile.name ) );
}

void ConnectionDialog::deleteProfile()
{
    const auto name = profileCombo_->currentText().trimmed();
    auto profiles = loadProfiles( settingsFile() );
    const auto removed = profiles.removeIf( [ & ]( const auto& p ) { return p.name == name; } );
    if ( removed == 0 ) {
        statusLabel_->setText( QStringLiteral( "There is no saved profile %1." ).arg( name ) );
        return;
    }
    if ( !saveProfiles( settingsFile(), profiles ) ) {
        statusLabel_->setText( QStringLiteral( "The profile %1 could not be deleted from %2." )
                                   .arg( name, settingsFile() ) );
        return;
    }
    fillProfileList( {} );
    statusLabel_->setText( QStringLiteral( "Profile %1 deleted." ).arg( name ) );
}

QString ConnectionDialog::missingSetting() const
{
    const auto profile = currentProfile();
    if ( profile.url.isEmpty() ) {
        return QStringLiteral( "Enter the server URL." );
    }
    const auto subjects = parseSubscriptions( subjectsEdit_->toPlainText() );
    if ( !subjects.error.isEmpty() ) {
        return subjects.error;
    }
    if ( subjects.subscriptions.isEmpty() ) {
        return QStringLiteral( "Enter the subject to capture." );
    }
    const auto& method = authMethodInfo( profile.auth.method );
    if ( method.takesUser && profile.auth.user.isEmpty() ) {
        return QStringLiteral( "Enter the user name." );
    }
    if ( method.secret && profile.auth.secret.isEmpty() ) {
        return QStringLiteral( "Enter the %1 (it is never saved)." )
            .arg( QLatin1String( method.secret ) );
    }
    if ( method.file && profile.auth.file.isEmpty() ) {
        return QStringLiteral( "Choose the %1." ).arg( QLatin1String( method.file ) );
    }
    if ( usesTls( profile.url, profile.tls.enabled )
         && profile.tls.certFile.isEmpty() != profile.tls.keyFile.isEmpty() ) {
        return QStringLiteral( "Choose both the client certificate and its key, or neither." );
    }
    return {};
}

void ConnectionDialog::browse( QLineEdit* edit, const QString& title )
{
    const auto path = QFileDialog::getOpenFileName( this, title, edit->text() );
    if ( !path.isEmpty() ) {
        edit->setText( path );
    }
}

ConnectionDialog::~ConnectionDialog()
{
    stopAll();
}

void ConnectionDialog::stopAll()
{
    if ( sessions_.isEmpty() ) {
        return;
    }
    // Stopped one after the other: each waits for its writer.
    while ( !sessions_.isEmpty() ) {
        auto* session = sessions_.first();
        session->stop();
        releaseSession( session );
    }
    statusLabel_->setText( QStringLiteral( "Stopped." ) );
}

bool ConnectionDialog::isCapturing() const
{
    return !sessions_.isEmpty();
}

void ConnectionDialog::startCapture()
{
    if ( const auto missing = missingSetting(); !missing.isEmpty() ) {
        statusLabel_->setText( missing );
        return;
    }

    const auto profile = currentProfile();
    SessionConfig config;
    config.url = profile.url;
    config.auth = profile.auth;
    if ( usesTls( profile.url, profile.tls.enabled ) ) {
        config.tls = profile.tls; // also for a tls:// URL with Use TLS unchecked
    }
    config.subscriptions = parseSubscriptions( subjectsEdit_->toPlainText() ).subscriptions;
    config.format.maxBinaryBytes = maxBinarySpin_->value();
    config.pendingLimits = { pendingMessagesSpin_->value(),
                             static_cast<int>( pendingMegabytesSpin_->value() * kBytesPerMiB ) };
    config.maxFileBytes = maxFileSpin_->value() * kBytesPerMiB;

    saveMaxFileMiB( maxFileSpin_->value() );
    if ( config.tls.skipVerify ) {
        hostLog( LOGSQUIRL_LOG_WARNING,
                 QStringLiteral( "NATS Monitor: the certificate of %1 is not verified (insecure)." )
                     .arg( redactUrls( config.url ) ) );
    }

    const auto subjects = describeSubscriptions( config.subscriptions );
    auto* session = new NatsSession( config, this );
    // With several sessions running, the status line says whose event it is.
    connect( session, &NatsSession::statusChanged, statusLabel_,
             [ label = statusLabel_, subjects ]( const QString& text ) {
                 label->setText( QStringLiteral( "%1: %2" ).arg( subjects, text ) );
             } );
    connect( session, &NatsSession::errorOccurred, this, [ subjects ]( const QString& message ) {
        const auto text = QStringLiteral( "NATS Monitor (%1): %2" ).arg( subjects, message );
        hostLog( LOGSQUIRL_LOG_WARNING, text );
        hostNotify( text );
    } );
    // Ended on its own: the status line shows why; the tab keeps what was
    // captured.
    connect( session, &NatsSession::finished, this,
             [ this, session ] { releaseSession( session ); } );
    // In the session's context, not the dialog's: releaseSession() keeps
    // it, so a file the session goes on in while it stops still gets its
    // tab (the queued signal comes before the session's deleteLater()).
    connect( session, &NatsSession::fileRotated, session,
             [ session ]( const QString& path ) { openRotatedFile( session->config(), path ); } );

    statusLabel_->setText(
        QStringLiteral( "%1: connecting to %2…" ).arg( subjects, redactUrls( config.url ) ) );
    if ( !session->start() ) {
        // start() has said why, in the status line and the log
        delete session;
        return;
    }
    sessions_ << session;
    sessionList_->addItem( QStringLiteral( "%1 on %2" ).arg( subjects, redactUrls( config.url ) ) );
    sessionList_->setCurrentRow( sessionList_->count() - 1 );
    updateButtons();

    if ( g_state.api && g_state.handle ) {
        g_state.api->open_file( g_state.handle, session->filePath().toUtf8().constData(), 1 );
    }
    hostNotify( QStringLiteral( "NATS capture of %1 started on %2." )
                    .arg( subjects, redactUrls( config.url ) ) );
}

void ConnectionDialog::stopSelected()
{
    const int row = sessionList_->currentRow();
    if ( row >= 0 && row < sessions_.size() ) {
        stopCapture( sessions_.at( row ) );
    }
}

void ConnectionDialog::stopAllCaptures()
{
    while ( !sessions_.isEmpty() ) {
        stopCapture( sessions_.first() );
    }
}

void ConnectionDialog::stopCapture( NatsSession* session )
{
    const auto subjects = describeSubscriptions( session->config().subscriptions );
    session->stop();
    releaseSession( session );
    statusLabel_->setText( QStringLiteral( "%1: stopped." ).arg( subjects ) );
    hostNotify( QStringLiteral( "NATS capture of %1 stopped." ).arg( subjects ) );
}

void ConnectionDialog::releaseSession( NatsSession* session )
{
    const auto row = sessions_.indexOf( session );
    if ( row < 0 ) {
        return; // released already
    }
    sessions_.removeAt( row );
    delete sessionList_->takeItem( static_cast<int>( row ) );
    session->preserveFile();
    session->disconnect( this );
    session->disconnect( statusLabel_ );
    session->deleteLater();
    updateButtons();
}

void ConnectionDialog::updateSubjectsWarning()
{
    const auto warning
        = overlapWarning( parseSubscriptions( subjectsEdit_->toPlainText() ).subscriptions );
    subjectsWarning_->setText( warning );
    subjectsWarning_->setVisible( !warning.isEmpty() );
}

void ConnectionDialog::updateButtons()
{
    stopButton_->setEnabled( sessionList_->currentRow() >= 0 );
    stopAllButton_->setEnabled( !sessions_.isEmpty() );
    const auto& method = chosenMethod( authCombo_ );
    userEdit_->setEnabled( method.takesUser );
    secretEdit_->setEnabled( method.secret );
    secretEdit_->setPlaceholderText(
        method.secret
            ? QStringLiteral( "The %1, never saved" ).arg( QLatin1String( method.secret ) )
            : QString() );
    authFileEdit_->setEnabled( method.file );
    authFileButton_->setEnabled( method.file );
    const bool tls = usesTls( urlEdit_->text(), tlsCheck_->isChecked() );
    for ( auto* widget : tlsWidgets_ ) {
        widget->setEnabled( tls );
    }
}

} // namespace nats_monitor

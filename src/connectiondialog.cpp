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
#include "sessions.h"
#include "subjects.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
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

/// Where the size limit is saved.
constexpr const char* kMaxFileMiBKey = "capture/maxFileMiB";

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
    return ok && value >= 0 && value <= kMaxFileMiBLimit ? value : kDefaultMaxFileMiB;
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

/// The height of @p area for @p lines lines of its font, with @p margin
/// around them.
int heightForLines( const QAbstractScrollArea* area, int lines, int margin = 0 )
{
    return area->fontMetrics().lineSpacing() * lines + 2 * ( area->frameWidth() + margin );
}

/// The method chosen in @p combo.
const AuthMethodInfo& chosenMethod( const QComboBox* combo )
{
    return authMethodInfo( static_cast<AuthMethod>( combo->currentData().toInt() ) );
}

} // namespace

ConnectionDialog::ConnectionDialog( const Sessions* sessions, QWidget* parent )
    : QDialog( parent )
    , sessions_( sessions )
    , profileCombo_( new QComboBox( this ) )
    , saveProfileButton_( new QPushButton( QStringLiteral( "Save" ), this ) )
    , deleteProfileButton_( new QPushButton( QStringLiteral( "Delete" ), this ) )
    , urlEdit_( new QLineEdit( QString::fromLatin1( kDefaultUrl ), this ) )
    , authCombo_( new QComboBox( this ) )
    , userEdit_( new QLineEdit( this ) )
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
    , statusLabel_( new QLabel( this ) )
{
    setWindowTitle( QStringLiteral( "NATS connection" ) );

    profileCombo_->setObjectName( "profile" );
    profileCombo_->setEditable( true );
    profileCombo_->setInsertPolicy( QComboBox::NoInsert );
    profileCombo_->lineEdit()->setPlaceholderText( QStringLiteral( "Name of the connection" ) );
    profileCombo_->setToolTip(
        QStringLiteral( "Enter a name and Save the settings below under it: the connection gets "
                        "a tile in the NATS sidebar, where it is started. Choose a saved "
                        "connection to change it. Passwords and tokens are never saved" ) );
    saveProfileButton_->setObjectName( "saveProfile" );
    saveProfileButton_->setDefault( true );
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
    subjectsEdit_->setFixedHeight( heightForLines(
        subjectsEdit_, 4, static_cast<int>( subjectsEdit_->document()->documentMargin() ) ) );
    subjectsWarning_->setObjectName( "subjectsWarning" );
    subjectsWarning_->setWordWrap( true );
    subjectsWarning_->hide();
    maxBinarySpin_->setObjectName( "maxBinaryBytes" );
    maxBinarySpin_->setRange( 0, kMaxBinaryBytesLimit );
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
    maxFileSpin_->setRange( 0, kMaxFileMiBLimit );
    maxFileSpin_->setGroupSeparatorShown( true );
    maxFileSpin_->setSuffix( QStringLiteral( " MiB" ) );
    maxFileSpin_->setSpecialValueText( QStringLiteral( "No limit" ) );
    maxFileSpin_->setValue( savedMaxFileMiB() );
    maxFileSpin_->setToolTip(
        QStringLiteral( "When a file reaches this size, the capture goes on in a new file, "
                        "opened in a new tab; a new connection proposes the last one saved" ) );
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
    connection->addRow( QStringLiteral( "Name:" ), profileRow );
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

    auto* layout = new QVBoxLayout( this );
    layout->addLayout( connection );
    layout->addWidget( authBox );
    layout->addWidget( tlsBox );
    layout->addLayout( form );
    layout->addWidget( statusLabel_ );

    connect( subjectsEdit_, &QPlainTextEdit::textChanged, this,
             &ConnectionDialog::updateSubjectsWarning );
    connect( profileCombo_, &QComboBox::currentIndexChanged, this, &ConnectionDialog::loadProfile );
    connect( saveProfileButton_, &QPushButton::clicked, this, &ConnectionDialog::saveProfile );
    connect( deleteProfileButton_, &QPushButton::clicked, this,
             [ this ] { deleteConnection( profileCombo_->currentText().trimmed() ); } );
    connect( authCombo_, &QComboBox::currentIndexChanged, this, &ConnectionDialog::updateButtons );
    connect( tlsCheck_, &QCheckBox::toggled, this, &ConnectionDialog::updateButtons );
    connect( urlEdit_, &QLineEdit::textChanged, this, &ConnectionDialog::updateButtons );

    newConnection();
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
    if ( method.file ) {
        profile.auth.file = authFileEdit_->text().trimmed();
    }
    profile.tls.enabled = tlsCheck_->isChecked();
    for ( const auto& [ edit, member ] : tlsTexts_ ) {
        profile.tls.*member = edit->text().trimmed();
    }
    profile.tls.skipVerify = skipVerifyCheck_->isChecked();
    profile.subjects = subjectsEdit_->toPlainText();
    profile.maxBinaryBytes = maxBinarySpin_->value();
    profile.pendingLimits = { pendingMessagesSpin_->value(),
                              static_cast<int>( pendingMegabytesSpin_->value() * kBytesPerMiB ) };
    profile.maxFileMiB = maxFileSpin_->value();
    return profile;
}

void ConnectionDialog::showProfile( const ConnectionProfile& profile )
{
    urlEdit_->setText( profile.url );
    authCombo_->setCurrentIndex( authCombo_->findData( static_cast<int>( profile.auth.method ) ) );
    userEdit_->setText( profile.auth.user );
    authFileEdit_->setText( profile.auth.file );
    tlsCheck_->setChecked( profile.tls.enabled );
    for ( const auto& [ edit, member ] : tlsTexts_ ) {
        edit->setText( profile.tls.*member );
    }
    skipVerifyCheck_->setChecked( profile.tls.skipVerify );
    subjectsEdit_->setPlainText( profile.subjects );
    maxBinarySpin_->setValue( profile.maxBinaryBytes );
    pendingMessagesSpin_->setValue( profile.pendingLimits.messages );
    pendingMegabytesSpin_->setValue(
        static_cast<int>( profile.pendingLimits.bytes / kBytesPerMiB ) );
    maxFileSpin_->setValue( profile.maxFileMiB );
    updateButtons();
}

void ConnectionDialog::newConnection()
{
    ConnectionProfile defaults;
    defaults.url = QString::fromLatin1( kDefaultUrl );
    defaults.maxFileMiB = savedMaxFileMiB();
    fillProfileList( {} );
    showProfile( defaults );
    statusLabel_->setText(
        QStringLiteral( "Enter a name and the settings, then Save: the connection gets a tile in "
                        "the NATS sidebar, where it is started." ) );
    profileCombo_->setFocus();
}

void ConnectionDialog::editConnection( const QString& name )
{
    fillProfileList( name );
    const int index = profileCombo_->findText( name );
    if ( index >= 0 ) {
        loadProfile( index );
    }
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
    const auto profile = findProfile( settingsFile(), name );
    if ( !profile ) {
        return;
    }
    showProfile( *profile );
    const char* secret = authMethodInfo( profile->auth.method ).secret;
    statusLabel_->setText(
        secret ? QStringLiteral( "Connection %1 loaded. Its %2 is asked for at Start." )
                     .arg( name, QLatin1String( secret ) )
               : QStringLiteral( "Connection %1 loaded." ).arg( name ) );
}

void ConnectionDialog::saveProfile()
{
    const auto profile = currentProfile();
    if ( profile.name.isEmpty() ) {
        statusLabel_->setText( QStringLiteral( "Enter a name for the connection to save." ) );
        return;
    }
    for ( const auto& url : profile.url.split( ',' ) ) {
        if ( !QUrl( url.trimmed() ).userInfo().isEmpty() ) {
            statusLabel_->setText(
                QStringLiteral( "The URL holds credentials, which a connection never saves: "
                                "enter them under Authentication instead." ) );
            return;
        }
    }
    if ( const auto problem = startProblem( profile ); !problem.isEmpty() ) {
        statusLabel_->setText( problem );
        return;
    }
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
        statusLabel_->setText( QStringLiteral( "The connection %1 could not be saved to %2." )
                                   .arg( profile.name, settingsFile() ) );
        return;
    }
    saveMaxFileMiB( profile.maxFileMiB );
    fillProfileList( profile.name );
    const char* secret = authMethodInfo( profile.auth.method ).secret;
    statusLabel_->setText(
        secret ? QStringLiteral( "Connection %1 saved, without the %2: start it in the NATS "
                                 "sidebar." )
                     .arg( profile.name, QLatin1String( secret ) )
               : QStringLiteral( "Connection %1 saved: start it in the NATS sidebar." )
                     .arg( profile.name ) );
    Q_EMIT profilesChanged();
}

bool ConnectionDialog::deleteConnection( const QString& name )
{
    if ( sessions_ && sessions_->isRunning( name ) ) {
        statusLabel_->setText(
            QStringLiteral( "The connection %1 is capturing: stop it to delete it." ).arg( name ) );
        return false;
    }
    auto profiles = loadProfiles( settingsFile() );
    const auto removed = profiles.removeIf( [ & ]( const auto& p ) { return p.name == name; } );
    if ( removed == 0 ) {
        statusLabel_->setText( QStringLiteral( "There is no saved connection %1." ).arg( name ) );
        return false;
    }
    if ( !saveProfiles( settingsFile(), profiles ) ) {
        statusLabel_->setText( QStringLiteral( "The connection %1 could not be deleted from %2." )
                                   .arg( name, settingsFile() ) );
        return false;
    }
    fillProfileList( {} );
    statusLabel_->setText( QStringLiteral( "Connection %1 deleted." ).arg( name ) );
    Q_EMIT profilesChanged();
    return true;
}

void ConnectionDialog::browse( QLineEdit* edit, const QString& title )
{
    const auto path = QFileDialog::getOpenFileName( this, title, edit->text() );
    if ( !path.isEmpty() ) {
        edit->setText( path );
    }
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
    const auto& method = chosenMethod( authCombo_ );
    userEdit_->setEnabled( method.takesUser );
    authFileEdit_->setEnabled( method.file );
    authFileButton_->setEnabled( method.file );
    const bool tls = usesTls( urlEdit_->text(), tlsCheck_->isChecked() );
    for ( auto* widget : tlsWidgets_ ) {
        widget->setEnabled( tls );
    }
}

} // namespace nats_monitor

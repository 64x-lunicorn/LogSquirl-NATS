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
 * @file profiles.cpp
 * @brief Implementation of the connection profiles and their store.
 */

#include "profiles.h"

#include "authmethods.h"
#include "natsoptions.h"
#include "replay.h"
#include "subjects.h"

#include <QSettings>
#include <QUrl>

#include <algorithm>
#include <array>
#include <limits>
#include <utility>

namespace nats_monitor {

namespace {

constexpr const char* kProfiles = "profiles";

/// Where the size limit saved last is remembered, outside the profiles:
/// 0.1.0 had it there already.
constexpr const char* kMaxFileMiBKey = "capture/maxFileMiB";

/// The text settings of TLS, by their keys in the group "tls".
const std::array<std::pair<const char*, QString TlsConfig::*>, 4> kTlsTexts{ {
    { "caFile", &TlsConfig::caFile },
    { "certFile", &TlsConfig::certFile },
    { "keyFile", &TlsConfig::keyFile },
    { "serverName", &TlsConfig::serverName },
} };

/// The switches of TLS, by their keys in the group "tls"; off by default.
const std::array<std::pair<const char*, bool TlsConfig::*>, 2> kTlsSwitches{ {
    { "enabled", &TlsConfig::enabled },
    { "skipVerify", &TlsConfig::skipVerify },
} };

AuthConfig readAuth( QSettings& settings )
{
    AuthConfig auth;
    settings.beginGroup( "auth" );
    const auto name = settings.value( "method" ).toString();
    for ( const auto& info : kAuthMethods ) {
        if ( name == QLatin1String( info.savedName ) ) {
            auth.method = info.method;
        }
    }
    auth.user = settings.value( "user" ).toString();
    auth.file = settings.value( "file" ).toString();
    settings.endGroup();
    return auth;
}

void writeAuth( QSettings& settings, const AuthConfig& auth )
{
    settings.beginGroup( "auth" );
    settings.setValue( "method", QString::fromLatin1( authMethodInfo( auth.method ).savedName ) );
    settings.setValue( "user", auth.user );
    settings.setValue( "file", auth.file );
    // Never auth.secret: the password or token is asked for at Start.
    settings.endGroup();
}

TlsConfig readTls( QSettings& settings )
{
    TlsConfig tls;
    settings.beginGroup( "tls" );
    for ( const auto& [ key, member ] : kTlsTexts ) {
        tls.*member = settings.value( key ).toString();
    }
    for ( const auto& [ key, member ] : kTlsSwitches ) {
        tls.*member = settings.value( key, false ).toBool();
    }
    settings.endGroup();
    return tls;
}

void writeTls( QSettings& settings, const TlsConfig& tls )
{
    settings.beginGroup( "tls" );
    for ( const auto& [ key, member ] : kTlsTexts ) {
        settings.setValue( key, tls.*member );
    }
    for ( const auto& [ key, member ] : kTlsSwitches ) {
        settings.setValue( key, tls.*member );
    }
    settings.endGroup();
}

/// @p settings' value @p key as an int in [@p min, @p max]; @p fallback if
/// it is missing or out of range.
int readInt( const QSettings& settings, const char* key, int min, int max, int fallback )
{
    bool ok = false;
    const int value = settings.value( key, fallback ).toInt( &ok );
    return ok && value >= min && value <= max ? value : fallback;
}

/// The subjects and limits, in the group "capture"; a profile saved before
/// they were gets the defaults.
void readCapture( QSettings& settings, ConnectionProfile& profile )
{
    settings.beginGroup( "capture" );
    profile.subjects = settings.value( "subjects" ).toString();
    profile.maxBinaryBytes
        = readInt( settings, "maxBinaryBytes", 0, kMaxBinaryBytesLimit, kDefaultMaxBinaryBytes );
    profile.pendingLimits.messages
        = readInt( settings, "pendingMessages", 1, std::numeric_limits<int>::max(),
                   kDefaultPendingLimits.messages );
    profile.pendingLimits.bytes = readInt(
        settings, "pendingBytes", 1, std::numeric_limits<int>::max(), kDefaultPendingLimits.bytes );
    profile.maxFileMiB = readInt( settings, "maxFileMiB", 0, kMaxFileMiBLimit, kDefaultMaxFileMiB );
    settings.endGroup();
}

void writeCapture( QSettings& settings, const ConnectionProfile& profile )
{
    settings.beginGroup( "capture" );
    settings.setValue( "subjects", profile.subjects );
    settings.setValue( "maxBinaryBytes", profile.maxBinaryBytes );
    settings.setValue( "pendingMessages", profile.pendingLimits.messages );
    settings.setValue( "pendingBytes", profile.pendingLimits.bytes );
    settings.setValue( "maxFileMiB", profile.maxFileMiB );
    settings.endGroup();
}

/// The replay, in the group "replay"; none if the profile has no stream.
ReplayConfig readReplay( QSettings& settings )
{
    ReplayConfig replay;
    settings.beginGroup( "replay" );
    replay.stream = settings.value( "stream" ).toString();
    replay.filterSubject = settings.value( "filterSubject" ).toString();
    const auto start = settings.value( "start" ).toString();
    for ( const auto& info : kReplayStarts ) {
        if ( start == QLatin1String( info.savedName ) ) {
            replay.start = info.start;
        }
    }
    // A start that cannot be read is kept for the start check to refuse,
    // never taken for another (replayProblem(), replay.h); one not saved
    // keeps its default.
    if ( settings.contains( "lastMessages" ) ) {
        readLastMessages( replay, settings.value( "lastMessages" ).toString() );
    }
    if ( settings.contains( "sequence" ) ) {
        readStartSequence( replay, settings.value( "sequence" ).toString() );
    }
    readStartTime( replay, settings.value( "timeUs" ).toString() );
    replay.followLive = settings.value( "followLive", false ).toBool();
    settings.endGroup();
    return replay;
}

void writeReplay( QSettings& settings, const ReplayConfig& replay )
{
    settings.beginGroup( "replay" );
    settings.setValue( "stream", replay.stream );
    settings.setValue( "filterSubject", replay.filterSubject );
    settings.setValue( "start", QString::fromLatin1( replayStartInfo( replay.start ).savedName ) );
    // One that cannot be read stays so (readLastMessages(),
    // readStartSequence(), readStartTime()).
    if ( replay.unreadLastMessages.isEmpty() ) {
        settings.setValue( "lastMessages", replay.lastMessages );
    }
    else {
        settings.setValue( "lastMessages", replay.unreadLastMessages );
    }
    if ( replay.unreadSequence.isEmpty() ) {
        settings.setValue( "sequence", replay.sequence );
    }
    else {
        settings.setValue( "sequence", replay.unreadSequence );
    }
    if ( replay.unreadTime.isEmpty() ) {
        settings.setValue( "timeUs", replay.timeUs );
    }
    else {
        settings.setValue( "timeUs", replay.unreadTime );
    }
    settings.setValue( "followLive", replay.followLive );
    settings.endGroup();
}

} // namespace

QList<Subscription> ConnectionProfile::subscriptions() const
{
    return parseSubscriptions( subjects ).subscriptions;
}

bool ConnectionProfile::usesTls() const
{
    return nats_monitor::usesTls( url, tls.enabled );
}

namespace {

/// The rules of startProblem() on @p config, whose subscriptions were made
/// of a text with the error @p subjectsError (empty if none, or no text).
QString firstStartProblem( const SessionConfig& config, const QString& subjectsError )
{
    if ( config.url.isEmpty() ) {
        return QStringLiteral( "Enter the server URL." );
    }
    if ( config.replay.enabled() ) {
        if ( const auto problem = replayProblem( config.replay ); !problem.isEmpty() ) {
            return problem;
        }
    }
    else {
        if ( !subjectsError.isEmpty() ) {
            return subjectsError;
        }
        const auto blank = []( const Subscription& s ) { return s.subject.trimmed().isEmpty(); };
        if ( config.subscriptions.isEmpty()
             || std::any_of( config.subscriptions.begin(), config.subscriptions.end(), blank ) ) {
            return QStringLiteral( "Enter the subject to capture." );
        }
    }
    const auto& method = authMethodInfo( config.auth.method );
    if ( method.takesUser && config.auth.user.isEmpty() ) {
        return QStringLiteral( "Enter the user name." );
    }
    if ( method.file && config.auth.file.isEmpty() ) {
        return QStringLiteral( "Choose the %1." ).arg( QLatin1String( method.file ) );
    }
    if ( usesTls( config.url, config.tls.enabled )
         && config.tls.certFile.isEmpty() != config.tls.keyFile.isEmpty() ) {
        return QStringLiteral( "Choose both the client certificate and its key, or neither." );
    }
    return {};
}

} // namespace

QString startProblem( const ConnectionProfile& profile )
{
    return firstStartProblem( sessionConfig( profile ),
                              parseSubscriptions( profile.subjects ).error );
}

QString startProblem( const SessionConfig& config )
{
    return firstStartProblem( config, QString() );
}

QString saveProblem( const ConnectionProfile& profile )
{
    if ( profile.name.isEmpty() ) {
        return QStringLiteral( "Enter a name for the connection to save." );
    }
    if ( const auto problem = replaySaveProblem( profile.replayChosen, profile.replay );
         !problem.isEmpty() ) {
        return problem;
    }
    for ( const auto& url : profile.url.split( ',' ) ) {
        if ( !QUrl( url.trimmed() ).userInfo().isEmpty() ) {
            return QStringLiteral( "The URL holds credentials, which a connection never saves: "
                                   "enter them under Authentication instead." );
        }
    }
    return startProblem( profile );
}

SessionConfig sessionConfig( const ConnectionProfile& profile )
{
    SessionConfig config;
    config.url = profile.url;
    config.auth = profile.auth;
    if ( profile.usesTls() ) {
        config.tls = profile.tls; // also for a tls:// URL with Use TLS unchecked
    }
    config.subscriptions = profile.subscriptions();
    config.replay = profile.replay;
    config.format.maxBinaryBytes = profile.maxBinaryBytes;
    config.pendingLimits = profile.pendingLimits;
    config.maxFileBytes = profile.maxFileMiB * kBytesPerMiB;
    return config;
}

ProfileStore::ProfileStore( const QString& settingsFile, IsRunning isRunning, QObject* parent )
    : QObject( parent )
    , settingsFile_( settingsFile )
    , isRunning_( std::move( isRunning ) )
{
}

QString ProfileStore::settingsFile() const
{
    return settingsFile_;
}

QList<ConnectionProfile> ProfileStore::list() const
{
    QList<ConnectionProfile> profiles;
    if ( settingsFile_.isEmpty() ) {
        return profiles;
    }
    QSettings settings( settingsFile_, QSettings::IniFormat );
    const int count = settings.beginReadArray( kProfiles );
    for ( int i = 0; i < count; ++i ) {
        settings.setArrayIndex( i );
        ConnectionProfile profile;
        profile.name = settings.value( "name" ).toString();
        if ( profile.name.isEmpty() ) {
            continue;
        }
        profile.url = settings.value( "url" ).toString();
        profile.auth = readAuth( settings );
        profile.tls = readTls( settings );
        readCapture( settings, profile );
        profile.replay = readReplay( settings );
        profile.replayChosen = profile.replay.enabled();
        profiles << profile;
    }
    settings.endArray();
    return profiles;
}

std::optional<ConnectionProfile> ProfileStore::find( const QString& name ) const
{
    for ( const auto& profile : list() ) {
        if ( profile.name == name ) {
            return profile;
        }
    }
    return std::nullopt;
}

ConnectionProfile ProfileStore::newProfile() const
{
    ConnectionProfile profile;
    profile.url = QString::fromLatin1( kDefaultUrl );
    if ( !settingsFile_.isEmpty() ) {
        const QSettings settings( settingsFile_, QSettings::IniFormat );
        profile.maxFileMiB
            = readInt( settings, kMaxFileMiBKey, 0, kMaxFileMiBLimit, kDefaultMaxFileMiB );
    }
    return profile;
}

QString ProfileStore::upsert( const ConnectionProfile& profile )
{
    if ( const auto problem = saveProblem( profile ); !problem.isEmpty() ) {
        return problem;
    }
    auto profiles = list();
    const auto existing = std::find_if( profiles.begin(), profiles.end(),
                                        [ & ]( const auto& p ) { return p.name == profile.name; } );
    if ( existing != profiles.end() ) {
        *existing = profile;
    }
    else {
        profiles << profile;
    }
    if ( !write( profiles, profile.maxFileMiB ) ) {
        return QStringLiteral( "The connection %1 could not be saved to %2." )
            .arg( profile.name, settingsFile_ );
    }
    Q_EMIT changed();
    return {};
}

QString ProfileStore::remove( const QString& name )
{
    if ( isRunning_ && isRunning_( name ) ) {
        // As saved: a replay saved as a capture while it runs is named a capture.
        const auto running = find( name );
        return QStringLiteral( "The connection %1 is %2: stop it to delete it." )
            .arg( name, running && running->replay.enabled() ? QStringLiteral( "replaying" )
                                                             : QStringLiteral( "capturing" ) );
    }
    auto profiles = list();
    if ( profiles.removeIf( [ & ]( const auto& p ) { return p.name == name; } ) == 0 ) {
        return QStringLiteral( "There is no saved connection %1." ).arg( name );
    }
    if ( !write( profiles, std::nullopt ) ) {
        return QStringLiteral( "The connection %1 could not be deleted from %2." )
            .arg( name, settingsFile_ );
    }
    Q_EMIT changed();
    return {};
}

bool ProfileStore::write( const QList<ConnectionProfile>& profiles, std::optional<int> maxFileMiB )
{
    if ( settingsFile_.isEmpty() ) {
        return false;
    }
    QSettings settings( settingsFile_, QSettings::IniFormat );
    if ( maxFileMiB ) {
        settings.setValue( kMaxFileMiBKey, *maxFileMiB );
    }
    settings.remove( kProfiles );
    settings.beginWriteArray( kProfiles, static_cast<int>( profiles.size() ) );
    for ( int i = 0; i < profiles.size(); ++i ) {
        const auto& profile = profiles.at( i );
        settings.setArrayIndex( i );
        settings.setValue( "name", profile.name );
        settings.setValue( "url", profile.url );
        writeAuth( settings, profile.auth );
        writeTls( settings, profile.tls );
        writeCapture( settings, profile );
        writeReplay( settings, profile.replay );
    }
    settings.endArray();
    settings.sync();
    return settings.status() == QSettings::NoError;
}

} // namespace nats_monitor

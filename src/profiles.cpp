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
 * @brief Implementation of the connection profiles.
 */

#include "profiles.h"

#include "authmethods.h"

#include <QSettings>

#include <array>
#include <utility>

namespace nats_monitor {

namespace {

constexpr const char* kProfiles = "profiles";

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

} // namespace

QList<ConnectionProfile> loadProfiles( const QString& settingsFile )
{
    QList<ConnectionProfile> profiles;
    if ( settingsFile.isEmpty() ) {
        return profiles;
    }
    QSettings settings( settingsFile, QSettings::IniFormat );
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
        profiles << profile;
    }
    settings.endArray();
    return profiles;
}

bool saveProfiles( const QString& settingsFile, const QList<ConnectionProfile>& profiles )
{
    if ( settingsFile.isEmpty() ) {
        return false;
    }
    QSettings settings( settingsFile, QSettings::IniFormat );
    settings.remove( kProfiles );
    settings.beginWriteArray( kProfiles, static_cast<int>( profiles.size() ) );
    for ( int i = 0; i < profiles.size(); ++i ) {
        const auto& profile = profiles.at( i );
        settings.setArrayIndex( i );
        settings.setValue( "name", profile.name );
        settings.setValue( "url", profile.url );
        writeAuth( settings, profile.auth );
        writeTls( settings, profile.tls );
    }
    settings.endArray();
    settings.sync();
    return settings.status() == QSettings::NoError;
}

} // namespace nats_monitor

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

#include <QSettings>

#include <array>
#include <utility>

namespace nats_monitor {

namespace {

constexpr const char* kProfiles = "profiles";

/// How each method is saved: words, so the file stays readable.
constexpr std::array<std::pair<AuthMethod, const char*>, 5> kMethods{ {
    { AuthMethod::None, "none" },
    { AuthMethod::UserPassword, "user" },
    { AuthMethod::Token, "token" },
    { AuthMethod::Credentials, "creds" },
    { AuthMethod::NKey, "nkey" },
} };

QString methodName( AuthMethod method )
{
    for ( const auto& [ value, name ] : kMethods ) {
        if ( value == method ) {
            return QString::fromLatin1( name );
        }
    }
    return QStringLiteral( "none" );
}

AuthMethod methodFromName( const QString& text )
{
    for ( const auto& [ value, name ] : kMethods ) {
        if ( text == QLatin1String( name ) ) {
            return value;
        }
    }
    return AuthMethod::None;
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
        profile.auth.method = methodFromName( settings.value( "auth/method" ).toString() );
        profile.auth.user = settings.value( "auth/user" ).toString();
        profile.auth.file = settings.value( "auth/file" ).toString();
        profile.tls.enabled = settings.value( "tls/enabled", false ).toBool();
        profile.tls.caFile = settings.value( "tls/caFile" ).toString();
        profile.tls.certFile = settings.value( "tls/certFile" ).toString();
        profile.tls.keyFile = settings.value( "tls/keyFile" ).toString();
        profile.tls.serverName = settings.value( "tls/serverName" ).toString();
        profile.tls.skipVerify = settings.value( "tls/skipVerify", false ).toBool();
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
        settings.setValue( "auth/method", methodName( profile.auth.method ) );
        settings.setValue( "auth/user", profile.auth.user );
        settings.setValue( "auth/file", profile.auth.file );
        // Never auth.secret: the password or token is asked for at Start.
        settings.setValue( "tls/enabled", profile.tls.enabled );
        settings.setValue( "tls/caFile", profile.tls.caFile );
        settings.setValue( "tls/certFile", profile.tls.certFile );
        settings.setValue( "tls/keyFile", profile.tls.keyFile );
        settings.setValue( "tls/serverName", profile.tls.serverName );
        settings.setValue( "tls/skipVerify", profile.tls.skipVerify );
    }
    settings.endArray();
    settings.sync();
    return settings.status() == QSettings::NoError;
}

} // namespace nats_monitor

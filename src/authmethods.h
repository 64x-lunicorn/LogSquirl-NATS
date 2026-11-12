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
 * @file authmethods.h
 * @brief What there is to know about each AuthMethod, in one table.
 *
 * The profiles save a method by its savedName, the dialog lists it by its
 * label and asks for what it takes (user, secret, file), and the
 * connection options and failure explanations (natsoptions.cpp) name its
 * file and what the server refused.  A new method is a row here and a case
 * in configureAuth() (natsoptions.cpp).
 */

#pragma once

#include "natssession.h"

#include <array>

namespace nats_monitor {

struct AuthMethodInfo {
    AuthMethod method;
    const char* savedName; ///< In a profile: a word, so nats.ini stays readable.
    const char* label;     ///< In the dialog's list.
    bool takesUser;        ///< Whether it takes AuthConfig::user.
    /// What AuthConfig::secret holds, for the user ("password"); nullptr if
    /// the method takes no secret.
    const char* secret;
    /// What AuthConfig::file is, for the user ("NKey seed file"); nullptr
    /// if the method reads no file.
    const char* file;
    /// What the server's refusal means with this method, for the user;
    /// "%1" is the file.
    const char* refused;
};

inline constexpr std::array<AuthMethodInfo, 5> kAuthMethods{ {
    { AuthMethod::None, "none", "No credentials", false, nullptr, nullptr,
      "the server requires authentication: choose how to log in" },
    { AuthMethod::UserPassword, "user", "User and password", true, "password", nullptr,
      "the server refused the user name or password" },
    { AuthMethod::Token, "token", "Token", false, "token", nullptr,
      "the server refused the token" },
    { AuthMethod::Credentials, "creds", "Credentials file (.creds)", false, nullptr,
      "credentials file", "the server refused the credentials in %1" },
    { AuthMethod::NKey, "nkey", "NKey seed file", false, nullptr, "NKey seed file",
      "the server refused the NKey in %1" },
} };

/// The row of kAuthMethods for @p method.
inline const AuthMethodInfo& authMethodInfo( AuthMethod method )
{
    for ( const auto& info : kAuthMethods ) {
        if ( info.method == method ) {
            return info;
        }
    }
    return kAuthMethods.front();
}

} // namespace nats_monitor

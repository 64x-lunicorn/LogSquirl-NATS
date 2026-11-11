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
 * @file fakehost.h
 * @brief In-process stand-in for the LogSquirl host API, for tests.
 *
 * A FakeHost hands the plugin a host API table that records every log
 * message, notification, opened file and menu entry for the test to
 * inspect.  The test passes api() and handle() to logsquirl_plugin_init().
 */

#pragma once

#include "logsquirl_plugin_api.h"

#include <catch2/catch.hpp>

#include <QList>
#include <QString>
#include <QStringList>

#include <stdexcept>

namespace nats_test {

class FakeHost {
public:
    /** A menu entry the plugin registered. */
    struct MenuAction {
        QString menuPath;
        QString label;
        void ( *callback )( void* userData );
        void* userData;

        /** Click the entry. */
        void trigger() const
        {
            callback( userData );
        }
    };

    FakeHost()
    {
        api_.api_version = LOGSQUIRL_PLUGIN_API_VERSION;
        api_.log_message = []( void* handle, int, const char* message ) {
            self( handle )->logs << QString::fromUtf8( message );
        };
        api_.show_notification = []( void* handle, const char* message ) {
            self( handle )->notifications << QString::fromUtf8( message );
        };
        api_.open_file = []( void* handle, const char* filePath, int ) {
            self( handle )->openedFiles << QString::fromUtf8( filePath );
        };
        api_.register_menu_action = []( void* handle, const char* menuPath, const char* label,
                                        void ( *callback )( void* ), void* userData ) {
            auto* host = self( handle );
            if ( host->failMenuAction ) {
                throw std::runtime_error( "the menu is full" );
            }
            host->menuActions.append(
                { QString::fromUtf8( menuPath ), QString::fromUtf8( label ), callback, userData } );
        };
    }

    FakeHost( const FakeHost& ) = delete;
    FakeHost& operator=( const FakeHost& ) = delete;

    /** The host API table, to pass to logsquirl_plugin_init(). */
    const LogSquirlHostApi* api() const
    {
        return &api_;
    }

    /** The plugin instance handle, to pass to logsquirl_plugin_init(). */
    void* handle()
    {
        return this;
    }

    QStringList logs;
    QStringList notifications;
    QStringList openedFiles;
    QList<MenuAction> menuActions;
    /// register_menu_action() throws, as a broken host might.
    bool failMenuAction = false;

private:
    static FakeHost* self( void* handle )
    {
        return static_cast<FakeHost*>( handle );
    }

    LogSquirlHostApi api_{};
};

} // namespace nats_test

// Let Catch print Qt strings in failure messages.
namespace Catch {
template <>
struct StringMaker<QString> {
    static std::string convert( const QString& value )
    {
        return '"' + value.toStdString() + '"';
    }
};
} // namespace Catch

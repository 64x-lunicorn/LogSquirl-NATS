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
 * @file sidebarwidget.h
 * @brief The plugin's tab "NATS" in LogSquirl's sidebar.
 *
 *   [New connection…]              [Stop all]
 *   ┌ tile: prod ───────────────────────────┐
 *   └───────────────────────────────────────┘
 *   ┌ tile: dev ────────────────────────────┐
 *   └───────────────────────────────────────┘
 *
 * One tile (connectiontile.h) per saved connection (profiles.h), in the
 * order they were saved, with what the connection is (server, how it logs
 * in, TLS, subjects) and a Start / Stop button.  While its session runs the
 * tile shows its state, msgs/s, received and dropped counts, the server it
 * is connected to and its file (name, size, how many files it rotated
 * into); after it failed, why.  Several connections can run at a time, one
 * session each (sessions.h).  All of that is the connection's status, which
 * Sessions keeps: the tab only maps it onto the tile (tileViewOf()).
 *
 * Start asks for the password or token when the connection's method takes
 * one: it is never saved.  Edit opens the dialog with the connection and New
 * connection… opens it empty; the plugin carries those out (editRequested()
 * and newConnectionRequested()).  Delete asks, then deletes the saved
 * connection through the profile store (profiles.h), not the dialog; when
 * the store cannot, a message box says why.  The tiles follow the store:
 * reload() shows the connections saved now whenever it changed.
 *
 * A tile is updated whenever Sessions says its connection's status changed:
 * at once on a transition, once a second while the counts move.
 *
 * Children named for tests: "newConnection", "stopAll", "empty" (the hint
 * shown without connections), and the tiles ("tile").
 *
 * OWNERSHIP
 * ─────────
 * Created by logsquirl_plugin_init() and registered with
 * register_sidebar_tab; the host parents it.  logsquirl_plugin_shutdown()
 * unregisters and deletes it, before the sessions it shows.
 */

#pragma once

#include "connectiontile.h"
#include "sessionstatus.h"

#include <QHash>
#include <QStringList>
#include <QWidget>

class QLabel;
class QPushButton;
class QVBoxLayout;

namespace nats_monitor {

class ProfileStore;
class Sessions;

/// What a connection's tile shows of its session.
struct TileView {
    TileState state = TileState::Stopped;
    QStringList details;
};

/// The tile view of @p status, one state for each SessionState, the
/// numbers in the locale's format: while running "running · <n> msgs/s",
/// the counts, the file and "connected to <server>"; while connecting
/// "connecting · <detail>"; after a failure "failed: <why>"; after a stop
/// (or a complete replay) the detail and what it captured.
TileView tileViewOf( const SessionStatus& status );

class SidebarWidget : public QWidget {
    Q_OBJECT

public:
    /// Shows the connections of @p profiles and the sessions of
    /// @p sessions, which must both outlive it.
    SidebarWidget( ProfileStore* profiles, Sessions* sessions, QWidget* parent = nullptr );

    /// Show the connections saved now.
    void reload();

Q_SIGNALS:
    /// New connection… was clicked.
    void newConnectionRequested();

    /// Edit was clicked on the connection @p name.
    void editRequested( const QString& name );

private:
    /// Start the connection @p name, asking for its secret if it takes one.
    void start( const QString& name );

    /// Ask whether to delete the connection @p name, and if so delete it,
    /// or say why it could not be.
    void confirmDelete( const QString& name );

    /// Show the status of the tile @p tile's connection.
    void updateTile( ConnectionTile* tile );

    /// Stop all is enabled while some capture runs.
    void updateStopAll();

    ProfileStore* profiles_;
    Sessions* sessions_;
    QPushButton* stopAll_;
    QLabel* empty_;
    QVBoxLayout* tiles_;
    QHash<QString, ConnectionTile*> tileByName_;
};

} // namespace nats_monitor

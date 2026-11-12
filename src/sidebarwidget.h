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
 * session each (sessions.h).
 *
 * Start asks for the password or token when the connection's method takes
 * one: it is never saved.  Edit opens the dialog with the connection, New
 * connection… opens it empty, and Delete asks, then deletes the saved
 * connection; the plugin carries those out (editRequested() and the
 * others), and reload() shows the connections saved now.
 *
 * The counts are polled once a second; a state change shows at once.
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

#include <QElapsedTimer>
#include <QHash>
#include <QWidget>

class QLabel;
class QPushButton;
class QTimer;
class QVBoxLayout;

namespace nats_monitor {

class ConnectionTile;
class Sessions;

class SidebarWidget : public QWidget {
    Q_OBJECT

public:
    /// Shows the saved connections and @p sessions, which must outlive it.
    explicit SidebarWidget( Sessions* sessions, QWidget* parent = nullptr );

    /// Show the connections saved now.
    void reload();

Q_SIGNALS:
    /// New connection… was clicked.
    void newConnectionRequested();

    /// Edit was clicked on the connection @p name.
    void editRequested( const QString& name );

    /// Delete was clicked on the connection @p name, and confirmed.
    void deleteRequested( const QString& name );

private:
    /// Start the connection @p name, asking for its secret if it takes one.
    void start( const QString& name );

    /// Ask whether to delete the connection @p name, and if so request it.
    void confirmDelete( const QString& name );

    /// Show what the session of the tile @p tile does.
    void updateTile( ConnectionTile* tile );

    /// The counts polled: the rate of each running session.
    void sample();

    void updateStopAll();

    Sessions* sessions_;
    QPushButton* stopAll_;
    QLabel* empty_;
    QVBoxLayout* tiles_;
    QTimer* timer_;
    QHash<QString, ConnectionTile*> tileByName_;

    /// What a running session had received at the last sample.
    struct Sample {
        quint64 received = 0;
        quint64 rate = 0; ///< msgs/s since the sample before.
        QElapsedTimer since;
    };
    QHash<QString, Sample> samples_;
};

} // namespace nats_monitor

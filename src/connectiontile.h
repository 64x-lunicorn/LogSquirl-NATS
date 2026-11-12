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
 * @file connectiontile.h
 * @brief One saved connection as a tile (card) in the sidebar.
 *
 *   ┌───────────────────────────────────────┐
 *   │ prod                       [▶ Start]  │  title, Start / Stop
 *   │ nats://a:4222 · User and password ·   │  summary: what the
 *   │ TLS · orders.>, jobs.*                │  connection is
 *   │ running · 1,234 msgs/s                │  details: what it does,
 *   │ received 5,000 · dropped 0 · …        │  while it runs or why it failed
 *   │                       [Edit] [Delete] │
 *   └───────────────────────────────────────┘
 *
 * A plain widget that knows nothing of NATS: the sidebar (sidebarwidget.h)
 * sets its texts and acts on its signals.  The same tile is meant for the
 * other LogSquirl plugins with saved connections (Serial ports, tcpdump
 * captures).
 *
 * Its children are named for tests: "title", "summary", "details",
 * "startStop", "edit", "delete".
 */

#pragma once

#include <QFrame>
#include <QString>
#include <QStringList>

class QLabel;
class QPushButton;

namespace nats_monitor {

class ConnectionTile : public QFrame {
    Q_OBJECT

public:
    /// A tile titled @p name, stopped, without a summary or details.
    explicit ConnectionTile( const QString& name, QWidget* parent = nullptr );

    /// The name of the connection it shows.
    QString name() const
    {
        return name_;
    }

    /// What the connection is, one line each.
    void setSummary( const QStringList& lines );

    /// What it does or did, one line each; none hides them.
    void setDetails( const QStringList& lines );

    /// Whether it runs: the button stops it then, and it cannot be deleted.
    void setRunning( bool running );

Q_SIGNALS:
    /// Start was clicked, the connection being stopped.
    void startClicked();
    /// Stop was clicked, the connection running.
    void stopClicked();
    /// Edit… was clicked.
    void editClicked();
    /// Delete was clicked (it cannot be while the connection runs).
    void deleteClicked();

private:
    QString name_;
    bool running_ = false;
    QLabel* summary_;
    QLabel* details_;
    QPushButton* startStop_;
    QPushButton* delete_;
};

} // namespace nats_monitor

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
 *   ╭───────────────────────────────────────╮
 *   │ ● prod                     [■ Stop]   │  state dot, title, Start / Stop
 *   │ nats://a:4222                         │  summary: what the
 *   │ User and password · TLS on            │  connection is
 *   │ Subjects  orders.>, jobs.*            │
 *   │ ───────────────────────────────────── │  separator, with the details
 *   │ running · 1,234 msgs/s                │  details (muted): what it does,
 *   │ received 5,000 · dropped 0 · …        │  while it runs or why it failed
 *   │                               [Edit…] │  Delete only while it is idle
 *   ╰───────────────────────────────────────╯
 *
 * A card: a rounded 1 px frame on a background set off the sidebar's, the
 * frame in the accent colour while the connection is active.  Its colours
 * are taken from the palette, so a light and a dark theme both work, and
 * follow a change of the palette.  The dot is grey (stopped), amber
 * (connecting, stopping), green (running) or red (failed); the details say
 * the same in words, for whoever cannot tell the colours apart.  While the
 * connection's session stops, which may take a while (a server that does
 * not answer), its button reads Stopping… and, like Edit…, is disabled.
 *
 * A plain widget that knows nothing of NATS: the sidebar (sidebarwidget.h)
 * sets its texts and state and acts on its signals.  The same tile is meant
 * for the other LogSquirl plugins with saved connections (Serial ports,
 * tcpdump captures).
 *
 * Its children are named for tests: "state" (its property "state" is
 * stopped, connecting, running, stopping or failed), "title", "summary",
 * "separator", "details", "startStop", "edit", "delete".
 */

#pragma once

#include <QFrame>
#include <QPointer>
#include <QString>
#include <QStringList>

class QEvent;
class QLabel;
class QPushButton;

namespace nats_monitor {

/// What a tile's connection does.
enum class TileState {
    Stopped,    ///< Idle: never started, stopped, or done.
    Connecting, ///< Started, not (yet, or again) connected.
    Running,    ///< Connected and capturing.
    Stopping,   ///< Stopped, not ended yet: nothing can be done until it has.
    Failed,     ///< Ended by an error.
};

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

    /// What it does.  While it connects or runs, the button stops it and
    /// it cannot be deleted (Delete is hidden).  While it is stopping, the
    /// button says so and is disabled, as is Edit…, and Delete is hidden.
    void setState( TileState state );

protected:
    void changeEvent( QEvent* event ) override;
    bool eventFilter( QObject* watched, QEvent* event ) override;

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
    /// Style the card from the palette around it, if that changed.
    void applyStyle();

    QString name_;
    bool active_ = false;
    /// The palette colours the stylesheet was made for.
    QString styledFor_;
    /// The parent whose palette changes it follows.
    QPointer<QWidget> watched_;
    QLabel* state_;
    QLabel* summary_;
    QFrame* separator_;
    QLabel* details_;
    QPushButton* startStop_;
    QPushButton* edit_;
    QPushButton* delete_;
};

} // namespace nats_monitor

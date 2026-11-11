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
 * @file statuswidget.h
 * @brief The plugin's widget in LogSquirl's status bar.
 *
 * It reads "NATS: 2 sessions · 1,234 msgs/s · 5 dropped": how many
 * sessions run, how many messages they receive a second (over the last
 * second), and how many the running sessions have dropped, slow consumer
 * and queue full together (each file marks where).  With no session
 * running it reads "NATS: no capture".
 *
 * It polls ConnectionDialog::totals() once a second, and updates the
 * session count at once when a session starts or ends.
 *
 * OWNERSHIP
 * ─────────
 * Created by logsquirl_plugin_init() and registered with
 * register_status_widget; logsquirl_plugin_shutdown() unregisters and
 * deletes it, before the dialog.
 */

#pragma once

#include "connectiondialog.h"

#include <QElapsedTimer>
#include <QLabel>

class QTimer;

namespace nats_monitor {

class StatusWidget : public QLabel {
    Q_OBJECT

public:
    /// Shows what @p dialog's sessions do; @p dialog must outlive it.
    explicit StatusWidget( const ConnectionDialog* dialog, QWidget* parent = nullptr );

private:
    /// Take the totals, and with @p sample the rate since the last sample.
    void refresh( bool sample );

    const ConnectionDialog* dialog_;
    QTimer* timer_;
    QElapsedTimer sinceSample_;
    quint64 receivedAtSample_ = 0;
    quint64 rate_ = 0; ///< msgs/s over the last sample.
};

} // namespace nats_monitor

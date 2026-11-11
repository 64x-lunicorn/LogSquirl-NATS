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
 * @file connectiondialog.h
 * @brief Non-modal dialog to start and stop a capture: server URL, subject.
 *
 * Opened from Plugins ▸ NATS Monitor…  It provides:
 *
 *   - the server URL (default nats://localhost:4222; several comma-separated)
 *   - the subject, with the wildcards * and >
 *   - how many bytes of a binary payload are written (default 256)
 *   - the subscription's pending limits, messages and MiB (default
 *     1,000,000 and 512 MiB): beyond them nats.c drops as a slow consumer
 *   - the size limit of a file in MiB (default 256, 0 for no limit): each
 *     next file of the capture opens in a new followed tab, with a line in
 *     the host log (no notification: a busy capture rotates often)
 *   - Start, which starts a NatsSession and opens its file in a followed tab
 *   - Stop, which stops it; the tab keeps the file until LogSquirl quits
 *   - a status line with the session's connection events and errors
 *
 * Starting, stopping with Stop and errors are also host notifications, as
 * in the Serial plugin; stopAll() (shutdown) is not.
 *
 * One session at a time (several sessions are #7's).
 *
 * SETTINGS
 * ────────
 * The size limit is remembered between sessions: Start saves it to
 * nats.ini in the plugin's configuration directory (get_config_dir), and
 * the dialog proposes it when it is created.  The other fields are not
 * saved (saved connections are #6's).
 *
 * OWNERSHIP
 * ─────────
 * Created by logsquirl_plugin_init(), deleted by logsquirl_plugin_shutdown(),
 * which calls stopAll() first.  The running session is a child of the dialog.
 */

#pragma once

#include <QDialog>

class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;

namespace nats_monitor {

class NatsSession;

class ConnectionDialog : public QDialog {
    Q_OBJECT

public:
    explicit ConnectionDialog( QWidget* parent = nullptr );
    ~ConnectionDialog() override;

    /// Stop the running session, keeping its file for the tab.
    void stopAll();

    /// Whether a session runs.
    bool isCapturing() const;

private:
    /// Start a session with the URL and subject entered, and open its file.
    void startCapture();

    /// Stop the running session and say so (Stop button).
    void stopCapture();

    /// The session ended on its own: it could not connect, or the
    /// connection was closed for good.
    void onSessionFinished();

    /// Let go of the session: keep its file, delete it later.
    void releaseSession();

    void updateButtons();

    QLineEdit* urlEdit_;
    QLineEdit* subjectEdit_;
    QSpinBox* maxBinarySpin_;
    QSpinBox* pendingMessagesSpin_;
    QSpinBox* pendingMegabytesSpin_;
    QSpinBox* maxFileSpin_;
    QPushButton* startButton_;
    QPushButton* stopButton_;
    QLabel* statusLabel_;
    NatsSession* session_ = nullptr;
};

} // namespace nats_monitor

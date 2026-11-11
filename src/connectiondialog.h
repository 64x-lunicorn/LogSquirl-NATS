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
 * @brief Non-modal dialog to start and stop captures: server URL, subjects.
 *
 * Opened from Plugins ▸ NATS Monitor…  It provides:
 *
 *   - the connection profile: choosing one fills in its settings, Save
 *     saves the settings under the name entered, Delete removes it
 *   - the server URL (default nats://localhost:4222; several comma-separated,
 *     the seeds of a cluster)
 *   - how to log in: no credentials, user and password, token, a .creds
 *     file or an NKey seed file
 *   - TLS: the CA file, a client certificate and its key, the server name
 *     the certificate must be for, and skipping the verification (off, and
 *     marked as insecure); they apply, and can be entered, when the
 *     connection uses TLS: Use TLS checked, or a tls:// URL
 *   - the subjects, one a line, with the wildcards * and >, each
 *     optionally followed by a queue group (subjects.h); a warning when
 *     they overlap, suggesting to drop the narrower one
 *   - how many bytes of a binary payload are written (default 256)
 *   - each subscription's pending limits, messages and MiB (default
 *     1,000,000 and 512 MiB): beyond them nats.c drops as a slow consumer
 *   - the size limit of a file in MiB (default 256, 0 for no limit): each
 *     next file of the capture opens in a new followed tab, with a line in
 *     the host log (no notification: a busy capture rotates often)
 *   - Start, which starts a NatsSession with the settings entered and opens
 *     its file in a followed tab
 *   - the list of the running sessions; Stop stops the one selected, Stop
 *     all every one; the tabs keep the files until LogSquirl quits
 *   - a status line with the sessions' connection events and errors, each
 *     with the subjects of its session
 *
 * Starting, stopping with Stop or Stop all and errors are also host
 * notifications, as in the Serial plugin; stopAll() (shutdown) is not.
 *
 * SEVERAL SESSIONS
 * ────────────────
 * Each Start starts another session beside those running, as the Serial
 * plugin does per port: its own connection, file and tab.  The settings
 * stay editable while sessions run, for the next one.
 *
 * SETTINGS
 * ────────
 * The size limit is remembered between sessions: Start saves it to
 * nats.ini in the plugin's configuration directory (get_config_dir), and
 * the dialog proposes it when it is created.  The connection settings are
 * saved by name, as profiles (profiles.h), in the same file.  A password
 * or token is never saved: the dialog asks for it at Start.
 *
 * ERRORS
 * ──────
 * A capture that cannot connect says why in the status line, in words the
 * user can act on: the server refused the password, the server's
 * certificate is not trusted, a file cannot be read, …  (natsoptions.h).
 *
 * OWNERSHIP
 * ─────────
 * Created by logsquirl_plugin_init(), deleted by logsquirl_plugin_shutdown(),
 * which calls stopAll() first.  The running sessions are children of the
 * dialog.
 */

#pragma once

#include "natssession.h"

#include <QDialog>
#include <QList>

#include <utility>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;

namespace nats_monitor {

class NatsSession;
struct ConnectionProfile;

class ConnectionDialog : public QDialog {
    Q_OBJECT

public:
    explicit ConnectionDialog( QWidget* parent = nullptr );
    ~ConnectionDialog() override;

    /// Stop every running session, keeping their files for the tabs.
    void stopAll();

    /// Whether a session runs.
    bool isCapturing() const;

private:
    /// Start a session with the settings entered, and open its file.
    void startCapture();

    /// Stop the session selected in the list and say so (Stop button).
    void stopSelected();

    /// Stop every session and say so (Stop all button).
    void stopAllCaptures();

    /// Stop @p session and say so.
    void stopCapture( NatsSession* session );

    /// Let go of @p session, which has stopped or ended on its own: keep
    /// its file, take it from the list, delete it later.
    void releaseSession( NatsSession* session );

    /// Show the warning for the subjects entered, if they overlap.
    void updateSubjectsWarning();

    void updateButtons();

    /// The connection settings entered, as a profile named as entered.
    ConnectionProfile currentProfile() const;

    /// Fill in the settings of the saved profile chosen in the list.
    void loadProfile( int index );

    /// Save the settings entered as the profile named in the list.
    void saveProfile();

    /// Remove the profile named in the list.
    void deleteProfile();

    /// Fill the list with the saved profiles' names, @p current chosen.
    void fillProfileList( const QString& current );

    /// What is missing for the settings entered to start a capture; empty
    /// if nothing.
    QString missingSetting() const;

    /// Choose a file for @p edit, titled @p title.
    void browse( QLineEdit* edit, const QString& title );

    QComboBox* profileCombo_;
    QPushButton* saveProfileButton_;
    QPushButton* deleteProfileButton_;
    QLineEdit* urlEdit_;
    QComboBox* authCombo_;
    QLineEdit* userEdit_;
    QLineEdit* secretEdit_;
    QLineEdit* authFileEdit_;
    QPushButton* authFileButton_;
    QCheckBox* tlsCheck_;
    QLineEdit* caFileEdit_;
    QLineEdit* certFileEdit_;
    QLineEdit* keyFileEdit_;
    QLineEdit* serverNameEdit_;
    QCheckBox* skipVerifyCheck_;
    QList<QWidget*> tlsWidgets_; ///< Enabled when the connection uses TLS.
    /// The TLS settings entered in a line edit, by their field.
    QList<std::pair<QLineEdit*, QString TlsConfig::*>> tlsTexts_;
    QPlainTextEdit* subjectsEdit_;
    QLabel* subjectsWarning_;
    QSpinBox* maxBinarySpin_;
    QSpinBox* pendingMessagesSpin_;
    QSpinBox* pendingMegabytesSpin_;
    QSpinBox* maxFileSpin_;
    QPushButton* startButton_;
    QListWidget* sessionList_;
    QPushButton* stopButton_;
    QPushButton* stopAllButton_;
    QLabel* statusLabel_;
    /// The running sessions, in the order of sessionList_'s rows.
    QList<NatsSession*> sessions_;
    bool fillingProfiles_ = false; ///< The list is being filled: nothing is loaded.
};

} // namespace nats_monitor

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
 * @brief Non-modal dialog for the settings of a saved connection.
 *
 * Opened from the sidebar (New connection…, a tile's Edit) and from
 * Plugins ▸ NATS Monitor…  It provides:
 *
 *   - the connection's name: choosing a saved one fills in its settings,
 *     Save saves the settings under the name entered (a new tile in the
 *     sidebar, or the tile of that name updated), Delete removes it
 *   - the server URL (default nats://localhost:4222; several comma-separated,
 *     the seeds of a cluster)
 *   - how to log in: no credentials, user and password, token, a .creds
 *     file or an NKey seed file; a password or token is asked for when the
 *     connection is started, and never saved
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
 *   - Replay a JetStream stream, instead of the subjects: the stream (List
 *     fetches the server's streams in the background), optionally one
 *     subject, where to start (all, the last N messages, a sequence, a UTC
 *     time) and whether to go on live once caught up (natssession.h, REPLAY)
 *   - a status line: what was saved, or what is missing
 *
 * The connections are started and stopped from their tiles in the sidebar
 * (sidebarwidget.h), not here.
 *
 * SETTINGS
 * ────────
 * The connections are saved by name, as profiles, through the profile store
 * (profiles.h), the one owner of nats.ini; the dialog reads and writes no
 * file itself.  Save also has the store remember the size limit, which a
 * new connection proposes.  The list of names follows the store, also when
 * a connection is deleted from its tile.
 *
 * OWNERSHIP
 * ─────────
 * Created by logsquirl_plugin_init(), deleted by logsquirl_plugin_shutdown().
 */

#pragma once

#include "sessionconfig.h"

#include <QDialog>
#include <QList>

#include <thread>
#include <utility>

class QCheckBox;
class QComboBox;
class QDateTimeEdit;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;

namespace nats_monitor {

class ProfileStore;
class Sessions;
struct ConnectionProfile;

class ConnectionDialog : public QDialog {
    Q_OBJECT

public:
    /// A dialog on the connections of @p profiles, which says when one it
    /// saves runs in @p sessions; both must outlive it.
    ConnectionDialog( ProfileStore* profiles, const Sessions* sessions,
                      QWidget* parent = nullptr );

    /// Waits for a listing of the streams still under way: at most its
    /// timeouts (listStreams() in streams.h).
    ~ConnectionDialog() override;

    /// Set up an empty new connection, with the default settings.
    void newConnection();

    /// Fill in the settings of the saved connection @p name.
    void editConnection( const QString& name );

private:
    /// Delete the saved connection @p name; the status line says what
    /// happened, or why it was not deleted.
    void deleteConnection( const QString& name );

    /// Show the warning for the subjects entered, if they overlap.
    void updateSubjectsWarning();

    void updateButtons();

    /// The settings entered, as a profile named as entered.
    ConnectionProfile currentProfile() const;

    /// Fill in the settings of @p profile.
    void showProfile( const ConnectionProfile& profile );

    /// Fill in the settings of the saved profile chosen in the list.
    void loadProfile( int index );

    /// Save the settings entered as the profile named in the list.
    void saveProfile();

    /// Fill the list with the saved profiles' names, @p current chosen.
    void fillProfileList( const QString& current );

    /// Choose a file for @p edit, titled @p title.
    void browse( QLineEdit* edit, const QString& title );

    /// List the server's streams into the stream list, in the background,
    /// asking for the password or token first if the method takes one.
    void listStreams();

    /// The streams were listed: offer @p names, or say what failed.
    void showStreams( const QStringList& names, const QString& error );

    ProfileStore* profiles_;
    const Sessions* sessions_;
    QComboBox* profileCombo_;
    QPushButton* saveProfileButton_;
    QPushButton* deleteProfileButton_;
    QLineEdit* urlEdit_;
    QComboBox* authCombo_;
    QLineEdit* userEdit_;
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
    QCheckBox* replayCheck_;
    QComboBox* replayStreamCombo_;
    QPushButton* listStreamsButton_;
    QLineEdit* replayFilterEdit_;
    QComboBox* replayStartCombo_;
    QSpinBox* replayLastSpin_;
    QLineEdit* replaySequenceEdit_;
    QDateTimeEdit* replayTimeEdit_;
    QCheckBox* replayFollowCheck_;
    QList<QWidget*> replayWidgets_; ///< Enabled when Replay is checked.
    QLabel* statusLabel_;
    bool fillingProfiles_ = false; ///< The list is being filled: nothing is loaded.
    std::thread listing_;          ///< Lists the streams (listStreams()).
    bool listingStreams_ = false;  ///< listing_ has yet to answer: List is disabled.
};

} // namespace nats_monitor

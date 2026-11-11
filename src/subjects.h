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
 * @file subjects.h
 * @brief The subjects of a session as the dialog takes them, and whether
 *        they overlap.
 *
 * The dialog takes one subscription a line, "<subject> [<queue group>]":
 * NATS subjects and queue group names contain no white space.
 *
 * Two subjects overlap when some subject matches both (orders.> and
 * orders.eu.*, or orders.*.paid and orders.eu.*).  The server then delivers
 * a message on it once for each subscription, and the session writes each
 * (natssession.h, SUBSCRIPTIONS), so the dialog warns.  One subject covers
 * another when every subject the other matches it matches too: then the
 * narrower one adds nothing but the copies, and the warning suggests to
 * drop it.  Matching follows NATS: tokens separated by '.', '*' matches one
 * token, '>' as the last token one or more.
 */

#pragma once

#include "natssession.h"

#include <QList>
#include <QString>

namespace nats_monitor {

/// What parseSubscriptions() made of the text entered.
struct ParsedSubscriptions {
    QList<Subscription> subscriptions;
    QString error; ///< Why a line cannot be taken; empty if all can.
};

/// The subscriptions of @p text, one a line, "<subject> [<queue group>]";
/// blank lines are skipped.
ParsedSubscriptions parseSubscriptions( const QString& text );

/// Whether some subject matches both @p a and @p b.
bool subjectsOverlap( const QString& a, const QString& b );

/// Whether every subject that matches @p narrower matches @p wider too.
bool subjectCovers( const QString& wider, const QString& narrower );

/// The warning the dialog shows for the overlapping subjects of
/// @p subscriptions, a sentence for each pair; empty if none overlap.
QString overlapWarning( const QList<Subscription>& subscriptions );

} // namespace nats_monitor

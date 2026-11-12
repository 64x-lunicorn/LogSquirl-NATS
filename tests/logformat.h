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
 * @file logformat.h
 * @brief The Log Format definition, formats/nats_log.json, as LogSquirl
 *        reads it: for logformat_test.cpp, which checks it, and for the
 *        line reader (capture.h), which reads a capture's lines with it.
 *
 * Its regexes are compiled with QRegularExpression and no options, its
 * columns are the named groups in pattern order, and the first pattern that
 * matches a line reads it, as in LogSquirl.
 */

#pragma once

#include <QJsonObject>
#include <QList>
#include <QRegularExpression>
#include <QString>
#include <QStringList>

namespace nats_test {

/// The one format the file defines, as LogSquirl reads it.
struct LogFormat {
    QString name;
    QJsonObject definition;
    QStringList patterns;
    QList<QRegularExpression> regexes; ///< The patterns, compiled with no options.
    QString timestampField = "timestamp"; // lnav's defaults, as LogSquirl's
    QString bodyField = "body";
    QStringList timestampFormats;
};

/// The Log Format read from formats/nats_log.json; fails the test if the
/// file holds anything but one format.
LogFormat loadLogFormat();

/// The Log Format, loaded once for the whole run.
const LogFormat& logFormat();

/// The format's match of @p line, the first of its patterns that matches;
/// no match if none does.
QRegularExpressionMatch matchLine( const LogFormat& format, const QString& line );

/// The named groups of @p pattern, by the rule LogSquirl derives the
/// column order with.
QStringList namedGroups( const QString& pattern );

} // namespace nats_test

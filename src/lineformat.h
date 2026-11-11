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
 * @file lineformat.h
 * @brief The line format: one NATS message, one line of the log file.
 *
 *     <UTC timestamp>             <subject>         <reply|-> <size> [<headers>] <payload>
 *     2026-11-11 10:00:00.123456Z orders.eu.created _INBOX.ab12 20 {"id":1,"total":9.5}
 *     2026-11-11 10:00:00.124001Z sensors.t1 - 4 [Nats-Msg-Id=42; Trace=abc] 21.5
 *
 * Fields are separated by one space.  Subjects contain no spaces (NATS
 * rules), so the fixed fields parse with a simple regex.
 *
 *   - Timestamp: receive time in UTC, with microseconds.
 *   - Size: the payload's real size in bytes, also when it is cut.
 *   - Headers, when there are any: `[k=v; k=v]`, in the order of
 *     Message::headers, with `;`, `]`, `\` and line breaks in keys and
 *     values escaped by a backslash, and `=` in keys too.
 *   - Payload, when there is one: valid UTF-8 as it is, with `\n`, `\r` and
 *     `\\` escaped, and the other C0 control characters but tab as `\xNN`
 *     (a NUL would make LogSquirl take the file for UTF-16), so that one
 *     message is always one line.  A leading `[` or `hex:` is escaped too
 *     (`\x5B`, `\x68ex:`), so that text never reads as headers or as a
 *     binary payload.  Anything else is `hex:` and lower-case hex digits,
 *     cut at FormatOptions::maxBinaryBytes.
 *
 * Marker lines (connection events, and later drops) use the subject
 * `$NATS-MONITOR`, which no client can publish to, in the same format.
 *
 * Pure functions: callable from any thread, nats.c's delivery threads
 * among them.
 */

#pragma once

#include <QByteArray>
#include <QList>

#include <utility>

namespace nats_monitor {

/// The subject of marker lines: no client can publish to it.
inline constexpr const char* kMarkerSubject = "$NATS-MONITOR";

/// Default for FormatOptions::maxBinaryBytes.
constexpr int kDefaultMaxBinaryBytes = 256;

/// A received message, as the formatter needs it.
struct Message {
    qint64 receivedUs = 0; ///< Receive time, microseconds since the epoch (UTC).
    QByteArray subject;
    QByteArray reply; ///< Empty: none.
    /// Header keys and values, in the order they are written; a key with
    /// several values appears once per value.
    QList<std::pair<QByteArray, QByteArray>> headers;
    QByteArray payload;
};

struct FormatOptions {
    /// How many bytes of a payload that is not UTF-8 are written (as hex).
    int maxBinaryBytes = kDefaultMaxBinaryBytes;
};

/// The line for @p message, without a line terminator.
QByteArray formatLine( const Message& message, const FormatOptions& options = {} );

/// A marker line for an event the plugin itself reports, without a line
/// terminator.  @p text is formatted as a payload.
QByteArray formatMarker( qint64 receivedUs, const QByteArray& text );

/// The current time in microseconds since the epoch, for Message::receivedUs.
qint64 nowUs();

} // namespace nats_monitor

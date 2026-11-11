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
 * @file nkey.h
 * @brief NATS NKeys: the public key of a user's seed.
 *
 * nats.c signs with the seed of an NKey seed file, but wants the user's
 * public key as well; this derives it, as nats.c reads the file.  Tested
 * through the session (auth_test.cpp: a seed the server knows logs in).
 */

#pragma once

#include <QByteArray>

namespace nats_monitor {

/**
 * The public key ("U…") of the first user seed ("SU…") in @p seedFile, the
 * content of an NKey seed file; empty if it holds none, or the seed is not
 * valid.
 */
QByteArray nkeyUserPublicKey( const QByteArray& seedFile );

} // namespace nats_monitor

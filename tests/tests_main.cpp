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
 * @file tests_main.cpp
 * @brief Catch2 + QApplication runner for the logsquirl-nats test suite.
 *
 * Qt widgets require a QApplication instance.  This translation unit
 * provides main(), creates the app with -platform offscreen, and
 * delegates to Catch2's session runner.
 *
 * Catch2 v2 cannot skip a test, so a test that needs a nats-server and has
 * none returns early through SKIP_WITHOUT_NATS_SERVER() (natsserver.h).  A
 * run in which tests were skipped and none failed exits with 77, which
 * ctest reports as skipped (SKIP_RETURN_CODE) rather than passed.
 */

// First: on Windows nats.h includes winsock2.h, which must come before any
// windows.h (Catch2's runner includes one).
#include <nats.h>

#define CATCH_CONFIG_RUNNER
#include <catch2/catch.hpp>

#include "natsserver.h"

#include <QApplication>

#include <iostream>

int main( int argc, char* argv[] )
{
    // Force offscreen rendering so tests run in headless CI environments.
    qputenv( "QT_QPA_PLATFORM", "offscreen" );

    QApplication app( argc, argv );

    const int result = Catch::Session().run( argc, argv );

    // nats.c's library-wide threads and memory.
    nats_Close();

    const int skipped = nats_test::skippedTests();
    if ( result == 0 && skipped > 0 ) {
        std::cout << "SKIPPED: " << skipped << " test(s) need a nats-server: "
                  << nats_test::NatsServer::unavailableReason().toStdString() << std::endl;
        return nats_test::kSkippedExitCode;
    }
    return result;
}

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
 * @file natsserver.h
 * @brief A real nats-server for the tests, started and stopped per test.
 *
 * The test build provides the binary (cmake/NatsServer.cmake).  A
 * NatsServer starts it on a free port of 127.0.0.1, optionally with
 * JetStream and a configuration (a file, or a text: ConfiguredServer),
 * waits until it accepts connections, and stops it when it goes out of
 * scope:
 *
 *     SCENARIO( "...", "[server]" )
 *     {
 *         SKIP_WITHOUT_NATS_SERVER();
 *         nats_test::NatsServer server;
 *         // connect to server.url()
 *     }
 *
 * Tests that need a server carry the tag [server]: ctest runs them as
 * logsquirl_nats_server_tests, reported as skipped when there is no server.
 */

#pragma once

#include <QString>

#include <memory>
#include <utility>

namespace nats_test {

/// The exit code of a run in which tests were skipped and none failed;
/// ctest reports it as skipped (SKIP_RETURN_CODE in tests/CMakeLists.txt).
constexpr int kSkippedExitCode = 77;

/// Count a test as skipped.  Use SKIP_WITHOUT_NATS_SERVER() instead.
void markSkipped();

/// How many tests were skipped in this run.
int skippedTests();

/// Whether the tests run in CI (the environment variable CI is set and not
/// 0/false/no/off): there a missing server fails the test, never skips it.
bool runningInCi();

struct NatsServerOptions {
    bool jetStream = false; ///< Start with JetStream (-js), its store in a temporary directory.
    QString configFile;     ///< A configuration file (-c); empty: none.
    /// A configuration, written to a file in the server's temporary
    /// directory, instead of configFile; empty: none.
    QString configText;
    int port = 0; ///< The port to listen on; 0: a free one.
};

/// The options of a server with JetStream, the others the defaults.
inline NatsServerOptions withJetStream()
{
    NatsServerOptions options;
    options.jetStream = true;
    return options;
}

class NatsServer {
public:
    /// Why there is no nats-server for the tests; empty if there is one.
    static QString unavailableReason();

    /// Whether there is a nats-server for the tests.
    static bool available()
    {
        return unavailableReason().isEmpty();
    }

    /// Start a server and wait until it accepts connections.
    /// Throws std::runtime_error, with the server's output, if it does not.
    explicit NatsServer( const NatsServerOptions& options = {} );

    /// Stop the server.
    ~NatsServer();

    NatsServer( const NatsServer& ) = delete;
    NatsServer& operator=( const NatsServer& ) = delete;

    /// The port it listens on, on 127.0.0.1.
    int port() const;

    /// Its client URL, nats://127.0.0.1:<port>.
    QString url() const;

private:
    struct Private;
    std::unique_ptr<Private> d;
};

/// A server started with the configuration @p configText (authorization,
/// TLS, operator mode…) and otherwise @p options: the fixture of the tests
/// that need a server set up.
class ConfiguredServer : public NatsServer {
public:
    explicit ConfiguredServer( const QString& configText, NatsServerOptions options = {} )
        : NatsServer( withText( std::move( options ), configText ) )
    {
    }

private:
    static NatsServerOptions withText( NatsServerOptions options, const QString& configText )
    {
        options.configText = configText;
        return options;
    }
};

} // namespace nats_test

/// In a test that needs a nats-server: if there is none, report the test
/// as skipped, with the reason, and end it here.  In CI, fail it instead.
#define SKIP_WITHOUT_NATS_SERVER()                                                                 \
    do {                                                                                           \
        if ( !nats_test::NatsServer::available() ) {                                               \
            if ( nats_test::runningInCi() ) {                                                      \
                FAIL( "needs a nats-server, and CI is set: "                                       \
                      << nats_test::NatsServer::unavailableReason().toStdString() );               \
            }                                                                                      \
            nats_test::markSkipped();                                                              \
            WARN( "SKIPPED: needs a nats-server: "                                                 \
                  << nats_test::NatsServer::unavailableReason().toStdString() );                   \
            return;                                                                                \
        }                                                                                          \
    } while ( false )

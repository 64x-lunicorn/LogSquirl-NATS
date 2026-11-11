# Developer Guide — LogSquirl NATS Monitor Plugin

How the plugin is built and tested. The architecture is in
[PLAN.md](PLAN.md).

## Building

```sh
cmake -B build -S . -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

Needs Qt 6 (Core, Widgets; Network for the tests), a C and C++ compiler,
Perl and make. On Windows: the MSVC environment (`nmake`) and a native Perl
such as Strawberry Perl — not the MSYS perl of Git for Windows; set
`OPENSSL_BUILD_PERL` if CMake picks the wrong one.

The first configure builds OpenSSL, which takes a few minutes (longest on
Windows, where nmake builds on one core). Later configures skip it while the
version, hash, options and compiler stay the same.

## Third-party code: OpenSSL and nats.c

Both are linked **statically** into `logsquirl_nats`, so the package carries
no other library and runs on every Linux distribution (no system OpenSSL).
Why: [ADR 1](adr/0001-static-openssl-built-by-the-plugin.md).

- **OpenSSL** (3.x LTS) is built by [`cmake/OpenSSL.cmake`](../cmake/OpenSSL.cmake)
  from the release tarball, at configure time: static only, no modules,
  engines, programs, tests or docs, no automatic loading of a system
  `openssl.cnf`; `no-asm` on Windows (no NASM). It defines `OpenSSL::SSL`
  and `OpenSSL::Crypto` and the variables `FindOpenSSL` reads, so nothing
  installed on the machine is picked up.
- **nats.c** comes through `FetchContent` ([`cmake/NatsC.cmake`](../cmake/NatsC.cmake))
  as the static `nats_static`, with TLS against that OpenSSL. Without
  libsodium (nats.c signs NKeys with its built-in ed25519), without NATS
  Streaming, examples and tests.

The library exports only the `logsquirl_plugin_*` entry points
(`-exported_symbol` on macOS, a version script and `--exclude-libs,ALL` on
Linux). On Linux this keeps the OpenSSL symbols local, so they never mix
with a system `libssl` the host process has loaded (Qt's TLS backend loads
one).

### A security fix in OpenSSL or nats.c

Versions and SHA-256s of every downloaded dependency (OpenSSL, nats.c, and
for the tests nats-server and Catch2) are in one place,
[`cmake/Dependencies.cmake`](../cmake/Dependencies.cmake).
Because both are linked in, **users get a fix only with a new plugin
release**:

1. Set the new version and its SHA-256 there (OpenSSL publishes a `.sha256`
   next to each tarball; for nats.c, hash GitHub's source archive of the tag).
2. Build and test on all three platforms (CI).
3. Release the plugin (bump `plugin.json` and `project(VERSION)`).

The ctest `no_shared_openssl` checks the library's dependencies (`otool -L`,
`objdump -p` or `dumpbin -dependents`): no OpenSSL shared library may appear.

### Licences in the package

Because the library contains Apache-2.0 code, the package carries `LICENSE`,
`NOTICE` and `Apache-2.0.txt` (from `LICENSES/`) beside the library:
`package_files` in `.github/plugin-ci.json`, and the install rule.

## Testing

Catch2 v2, one test program run as two ctest entries, plus a dependency check:

| ctest entry | Runs |
|---|---|
| `logsquirl_nats_tests` | everything that needs no server |
| `logsquirl_nats_server_tests` | the tests tagged `[server]`, against a real `nats-server` |
| `no_shared_openssl` | the plugin library links no OpenSSL shared library |

### The nats-server of the tests

A plugin has no workflows of its own, so the test build provides the server
itself ([`cmake/NatsServer.cmake`](../cmake/NatsServer.cmake)): configuring
with `BUILD_TESTS=ON` downloads the official `nats-server` release archive
for the platform (linux-amd64, darwin-arm64, windows-amd64) at the pinned
version, checks its SHA-256 and unpacks it into the build tree.

- `-DNATS_SERVER_EXECUTABLE=/path/to/nats-server` uses that binary instead.
- `-DNATS_SERVER_DOWNLOAD=OFF` downloads nothing.
- `-DNATS_SERVER_RELEASES_URL=<url>` downloads `<url>/v<version>/<archive>` from a mirror (the hash must match).

Without a server (offline, switched off, no build for the platform) the
build succeeds and `logsquirl_nats_server_tests` is reported by ctest as
**skipped** (exit code 77). The reason is a configure warning and in the
test's output (`ctest -V`: `SKIPPED: … need a nats-server: <reason>`).
A download with the wrong hash fails the configure.

**In CI** (the environment variable `CI` is set, as on GitHub Actions) there
is no skip: a missing server fails the configure, and a server test that
finds none fails. Otherwise the job would pass with the round trip and TLS
never tested.

In a test, `nats_test::NatsServer` ([`tests/natsserver.h`](../tests/natsserver.h))
starts a server on a free port of 127.0.0.1 — optionally with JetStream
and a configuration file — waits until it accepts connections, and stops it
when it goes out of scope:

```cpp
SCENARIO( "…", "[server]" )
{
    SKIP_WITHOUT_NATS_SERVER();
    nats_test::NatsServer server;   // or NatsServer( { /*jetStream*/ true, configFile } )
    // connect with nats.c to server.url()
}
```

`NatsServerOptions::port` starts it on a given port instead, e.g. to bring
a stopped server back for a reconnect test.

### The test seams

- **A capture session against a real `nats-server`** (`natssession_test.cpp`,
  and through the dialog in `plugin_test.cpp`): start a `NatsSession`,
  publish with `nats_test::Publisher`, and assert on the lines of
  `session.filePath()` (`nats_test::readLines`, `nats_test::waitFor` from
  [`tests/sessiontest.h`](../tests/sessiontest.h)). A session is subscribed
  once its first line, the `connected to …` marker, is in the file; from
  then on every message published is captured.
- **The formatter** (`lineformat_test.cpp`): `formatLine()` is a pure
  function from a message to one line.

### TLS test certificates

`tests/certs/` holds a test CA and a server certificate for `localhost` /
`127.0.0.1`, valid for 100 years, made by `tests/certs/generate.sh`. They
are committed, not generated per run, so the TLS test needs no `openssl`
program (the Windows runners have none) and behaves the same everywhere.
The keys protect nothing; the CA's key was thrown away.

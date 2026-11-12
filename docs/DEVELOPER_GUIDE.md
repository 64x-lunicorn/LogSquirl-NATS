# Developer Guide — LogSquirl NATS Monitor Plugin

How the plugin works, and how it is built and tested. The plan and the
decisions behind it are in [PLAN.md](PLAN.md); what the user sees is in the
[README](../README.md).

- [Architecture](#architecture): threads, queue, writer, rotation, and the
  features built on them
- [Building](#building), and [how nats.c and OpenSSL are built and
  linked](#third-party-code-openssl-and-natsc)
- [Testing](#testing)

## Architecture

```
plugin.cpp            entry points (logsquirl_plugin_*), each through guarded()
  │  registers        the sidebar tab "NATS" (register_sidebar_tab) and
  │                   Plugins → NATS Monitor…  (register_menu_action)
  ├─ SidebarWidget    the tab: New connection…, Stop all, a tile per saved connection
  │    └─ ConnectionTile × N  plain card: summary, details, Start/Stop, Edit, Delete
  ├─ ConnectionDialog edits and saves connections (profiles.h); opened from the tab
  └─ Sessions         one session per connection; state and counts for the tiles
       └─ NatsSession × N   children of Sessions
            ├─ nats.c threads   callbacks: format a line, enqueue
            ├─ queue            bounded, mutex + condition variable
            └─ writer thread    connects, writes in batches, rotates
```

### The plugin's lifetime

[`src/plugin.cpp`](../src/plugin.cpp) holds the entry points. No exception
may leave one, a host callback or a slot the host's event loop calls (the
host is C), so their work runs through `guarded()` (plugin.h), which logs
a failure to the host log instead.

- **init** removes the temporary directories of LogSquirl processes that no
  longer run, creates `Sessions`, the dialog (parentless: it must not die
  with a main window) and the sidebar tab, connects the tab's requests (new,
  edit, delete) to the dialog and the dialog's saves back to the tab,
  registers the menu action and the tab, and logs which nats.c and OpenSSL
  it carries. A failure is undone by shutdown, and init returns 1.
- **shutdown** unregisters and deletes the sidebar tab (before the sessions
  it shows; `PluginState` holds it in a `QPointer`, since the host parents
  it and a host window deleted first takes it along), deletes the dialog,
  stops every session (`Sessions::stopAll()`: files flushed and kept for
  the tabs) and deletes `Sessions`. Only when LogSquirl quits
  (it saw `aboutToQuit`), not when the plugin is disabled or updated with
  its tabs still open, does it remove this process's temporary directories.
  Last, `nats_CloseAndWait()` (5 s) waits for nats.c's threads, which run
  code of this library, to end before the host unloads it.
- **configure** has no settings yet; everything is in the tab and its dialog.

### Threads

A capture session, [`NatsSession`](../src/natssession.h), runs on three
kinds of thread:

| Thread | What it does |
|---|---|
| GUI thread | The tab, the dialog, `Sessions` and the `NatsSession` objects. Every signal of a session is emitted here, through queued calls. |
| The session's writer thread (`std::thread`, one per session) | Connects and subscribes (so `start()` never waits for the network), owns the file, writes, rotates, and on stop drains and closes. |
| nats.c's threads | Call the message callback of each subscription and the connection callbacks (disconnected, reconnected, closed, async error). They only format the line, with the pure `formatLine()` ([`src/lineformat.h`](../src/lineformat.h)), and enqueue it: they never touch the file or the GUI, and never wait for the writer. |

What the callbacks use (`Shared` in natssession.cpp) is kept apart from the
writer: should nats.c not confirm within its timeout that its callbacks are
over, `Shared` is leaked rather than freed under a running callback.

### The queue

The lines go from nats.c's threads to the writer through a `std::vector`
guarded by a `std::mutex` and a `std::condition_variable`. It is bounded
for messages: once it holds `SessionConfig::queueLimit` of them (100,000,
two seconds at 50k msgs/s), a new message is dropped and counted, and the
next message queued is preceded by its `dropped=<n> (queue full)` marker.
Marker lines (connection events, drops) are always queued.

Before that, nats.c holds up to each subscription's pending limits
(`SessionConfig::pendingLimits`, 1,000,000 messages and 512 MiB by default,
set with `natsSubscription_SetPendingLimits`) and drops beyond them as a
slow consumer. Each message callback, the slow-consumer error and the
disconnected callback read `natsSubscription_GetDropped` and queue a
`dropped=<n> (slow consumer)` marker for what is new. Where those markers
land, and how they are tested: [Dropped messages and the
benchmark](#dropped-messages-and-the-benchmark).

### The writer

The writer waits on the condition variable until lines are queued, stop is
requested or the connection is closed for good. It then swaps the whole
queue out, so it never holds the mutex while writing, writes the batch and
flushes once: LogSquirl, following the file, sees whole lines. Before each
line it checks the size limit (below). A write or flush that fails (disk
full, volume gone) is reported once, on the tile and as a
notification, and later lines are not written.

**Stopping.** `stop()` asks the writer to end and waits for it. The writer
lifts the pending limits, so nats.c drops nothing more, and marks the last
dropped counts. It drains each subscription (`natsSubscription_DrainTimeout`,
at most 15 s; not connected, it waits up to 5 s until nats.c holds nothing
for them),
writing all the while, closes the connection, waits for nats.c's last
callback, writes what is left and the `stopped` marker, and closes the
file. Nothing that reached the session before `stop()` is lost uncounted.

**Temporary files.** Each session writes into its own directory
`logsquirl-nats-<pid>-XXXXXX` in the system's temporary directory
([`src/tempdirs.h`](../src/tempdirs.h)), named `nats_<subjects>.log`.
`Sessions` calls `preserveFile()` when a session ends, so the tab keeps its
file; the plugin removes its directories when LogSquirl quits, and those of
processes that no longer run at the next init.

### Rotation at the size limit

`SessionConfig::maxFileBytes` (the dialog's *New file at*, in MiB; default
256, 0 for no limit) bounds each file of a capture. The writer thread
rotates by itself, between two lines: before a line that would take the
file over the limit it opens the next one (`NewOnly`, so it never
truncates an earlier file), closes the full one and reports the path with
`NatsSession::fileRotated()`, on which `Sessions` opens a new followed tab
and writes a line to the host log (not a notification, as in
LogSquirl-Serial: a busy capture rotates often). `filePath()` follows the
rotation. A session started again goes on in its last file, with what that
file holds counted against the limit. The rotation tests (`[rotation]`, part
of the server tests) use limits of a few hundred bytes, and make a rotation
fail by putting a directory where the next file goes, which works on every
platform and also for root.

The size limit is saved with the connection (its profile, below). Save
also writes it to `nats.ini` (`capture/maxFileMiB`) in the directory the
host's `get_config_dir` gives, and a new connection proposes it. The tests'
`FakeHost` gives each host a configuration directory of its own.

### Several subjects, several sessions

`SessionConfig::subscriptions` lists what a session subscribes to: one or
more `Subscription`s, a subject and an optional queue group (empty, the
default: a session in a queue group takes messages away from the group's
other members). All of them write into the session's one file,
`nats_<subjects>.log` (the subjects made safe for a file name, joined by
`_`, cut to 100 characters), and its first marker names them:
`connected to …, subscribed to orders.>, jobs.* (queue workers)`
(`describeSubscriptions()`). Pending limits, slow-consumer markers and
draining on stop are per subscription; `receivedCount()` and
`droppedCount()` are the session's.

**Overlapping subjects** are written as the server delivers them: nats.c
delivers a message that matches several subscriptions once for each, so
`orders.>` and `orders.eu.*` give two lines for `orders.eu.created`. No
deduplication, which would hide what the server sent. The dialog takes the
subjects one a line, `<subject> [<queue group>]`
([`src/subjects.h`](../src/subjects.h)), refuses at Start a subject NATS
would refuse (an empty token, `*` or `>` inside a token, `>` before the
last token), and warns below them when two
overlap (some subject matches both); when one covers the other (every
subject the narrower matches, the wider matches too) it suggests dropping
the narrower one.

**One session per saved connection.** `Sessions`
([`src/sessions.h`](../src/sessions.h)) starts a session for a connection's
profile, beside those of the other connections, with its own connection,
file and tab, as LogSquirl-Serial does per port; shutdown stops them all.
It keeps what the tile shows: the state (`SessionState`: connecting,
running, failed, stopped), the last connection event or why it failed, the
server it is connected to, its file and how many files it rotated into,
and the counts. That stays after the session ended, until the connection
is started again.

The tests (`[subjects]`) are at the main seam: two subjects and the overlap
in `natssession_test.cpp` (and a queue group shared with another member);
several connections and the overlap warning through the tab and the dialog
in `plugin_test.cpp`.

### JetStream replay

With `SessionConfig::replay` set (a `ReplayConfig` with a stream name), a
session replays the stream instead of subscribing to its subjects
([`src/natssession.h`](../src/natssession.h), REPLAY). The writer thread
connects, writes `connected to …, replaying stream ORDERS, …`
(`describeReplay()`, [`src/replay.h`](../src/replay.h), which also has
the start positions as one table, `kReplayStarts`, and the checks a replay
must pass, `replayProblem()`) and subscribes with `js_Subscribe` and
`jsSubOptions::Ordered`: nats.c's ordered push consumer, ephemeral, AckNone,
with flow control and heartbeats, which nats.c recreates from the last
stream sequence after a gap or a reconnect. The filter subject is the
`ReplayConfig::filterSubject`, or `>` for all of the stream's.

- **Start.** *All*: `js_DeliverAll`. *Sequence*: `js_DeliverByStartSequence`
  with `OptStartSeq`. *Time*: `js_DeliverByStartTime` with `OptStartTime`
  (nanoseconds). *Last N* without a filter: by start sequence, `OptStartSeq` =
  the stream's last sequence − N + 1, from `js_GetStreamInfo`. *Last N* with
  a filter: which messages match is the server's to know, so it delivers
  all and the callback skips while the message's `NumPending` (metadata)
  is N or more, until the first one written.
- **The line.** `onReplayMessage` reads `natsMsg_GetMetaData`: the stored
  time (`Timestamp`) and the stream sequence go into `Message`, and the
  reply is cleared (nats.c's is the consumer's; the stream keeps none).
- **Caught up.** A message with `NumPending` 0 is the last one there was:
  with `followLive` the writer writes `caught up, following live` and goes
  on; otherwise the writer ends as on stop, writes `replay complete` and
  the session emits `completed()` (not `finished()`, which is a failure),
  on which `Sessions` sets the tile to stopped with *replay complete*. With
  nothing to replay no message comes, so after subscribing the consumer's
  info (`natsSubscription_GetConsumerInfo`: nothing pending, nothing
  delivered) says it.
- **No drops by the plugin.** A replay's message callback waits for room in
  the queue instead of dropping (`Shared::waitForRoom`, woken when the
  writer takes the queue, and not while stopping): the stream holds the
  messages, and while nats.c's delivery thread waits it answers no flow
  control, so the server holds back. A stream of a million messages is
  written whole in about two seconds; stop ends it at once. A writer that
  cannot keep up misses no heartbeat: nats.c marks the ordered consumer
  active from its reader thread, on each message and heartbeat that
  arrives (`jsSub_trackSequences`, conn.c), not from the delivery thread
  that waits, so the consumer is not reset; a test replays 20,000 messages
  through a queue of one.
- **Retention.** Before subscribing, `js_GetStreamInfo` gives the stream's
  retention (and its first and last sequence, for *Last N*). A work queue
  (`js_WorkQueuePolicy`) is refused: the replay fails with `replaying
  stream JOBS failed: it is a work queue …`, before any consumer exists.
  An interest stream (`js_InterestPolicy`) gets a `warning: stream EVENTS
  has interest retention …` marker (a status event) and is replayed.
- **Stop** drains the subscription like a live one; for a consumer
  nats.c created, the drain (or unsubscribe) also deletes it on the server.
  Unload and quit stop the same way (`Sessions::stopAll()`). A lost
  connection keeps the consumer, so the replay resumes on reconnect; a
  Stop while disconnected leaves it to the inactive threshold (PLAN.md,
  Decisions).
- **Inactive threshold.** When the delete request cannot reach the server
  (a crash, the connection down at Stop) the consumer's
  `InactiveThreshold` (`ReplayConfig::inactiveThresholdMs`, 5 s by
  default; 0 leaves it to the server) removes it once nobody reads its
  deliver subject. nats.c copies the config when it recreates the ordered
  consumer, and the consumer it replaces goes the same way.

The streams the dialog offers come from `listStreams()` (replay.h;
it connects with `connectTo()`, as the session does, natsoptions.h;
`js_StreamNames`, `$JS.API.STREAM.NAMES`), run on a thread of the dialog;
List is disabled until it answers. The dialog joins it when it goes (at
shutdown, before the library may be unloaded), so the listing bounds
itself: 2 s to connect to each server URL and 2 s for each answer. The tests (`[replay]`) are at the main
seam, `replay_test.cpp`, against a server started with `-js`
(`NatsServerOptions::jetStream`); `nats_test::JetStream`
([`tests/sessiontest.h`](../tests/sessiontest.h)) adds memory streams,
publishes into them and reads their message count, stored times and
consumers. The million-message test (`[large]`) takes about ten seconds.

The cleanup tests (`[cleanup]`) start from a stream with a durable consumer
of its own and compare the stream's consumer list before and after Stop, a completed replay, unload and quit (`plugin_test.cpp`), a
connection cut and restored, and a Stop while the connection is down. The
connection is cut by `nats_test::TcpProxy`
([`tests/tcpproxy.h`](../tests/tcpproxy.h)), a proxy in its own thread in
front of the server: `cut()` closes the connections, `refuse()` accepts no
new ones. A crash is a real one: the test program starts itself as a child
process running the hidden test *a replay runs in a process of its own
until it is killed* (`[.][child]`, set up through environment variables),
kills it and waits for the consumer to go, with a threshold of 500 ms. The
permissions the README lists are checked by `permissions_test.cpp`
(`[permissions]`) against a server configured with exactly those.

### The sidebar tab and its tiles

The tab "NATS" ([`src/sidebarwidget.h`](../src/sidebarwidget.h)) is
registered with `register_sidebar_tab` at init and unregistered at
shutdown. It holds *New connection…*, *Stop all*, and one tile per saved
connection in the order they were saved, rebuilt when the dialog saves or
deletes one (`ConnectionDialog::profilesChanged`).

- **The tile** ([`src/connectiontile.h`](../src/connectiontile.h)) is a
  plain `QFrame` that knows nothing of NATS: a title, a summary, details,
  Start / Stop, Edit and Delete, with a signal for each button. The tab sets
  its texts. It is meant for Serial and tcpdump too.
- **Summary**: the URLs, the authentication method, TLS on or off (as
  `usesTls()`), the subjects.
- **Details**, polled once a second and updated at once on a state change
  (`Sessions::changed`): while running, `running · <n> msgs/s` (the rate
  over the last second), `received · dropped`, the file's name, size and
  number after a rotation, and `connected to <server>`; while connecting,
  the last event; after a failure, `failed: <why>`; after Stop, what it
  captured.
- **Start** reads the profile from `nats.ini` and, when its method takes a
  password or token, asks for it (`QInputDialog`, password echo); it is
  never saved, and an empty answer starts nothing. **Delete** asks
  (`QMessageBox`) and is disabled while the connection runs; the dialog
  refuses to delete a running connection too.
- **Edit** while the connection runs: the tile shows the saved settings at
  once, the capture runs on with those it started with (the dialog says
  so) until it is stopped and started again. A connection saved under
  another name is another connection, with a tile of its own.

The tests (`[sidebar]`) drive the tab through `FakeHost`, which records the
sidebar tabs registered and unregistered: tiles created, updated and
deleted, Start and Stop, the details of a running tile, two tiles at once,
and drops on a tile. They answer the password prompt and the delete
question by finding the modal dialog from a timer (`onNextModal()` in
`plugin_test.cpp`).

### The Log Format

`formats/nats_log.json` gives LogSquirl's Table View the columns
*timestamp, subject, reply, seq, size, body* (seq the stream sequence of a
replayed message, the body being the headers and
the payload). CI packages it beside the library (`package_files` in
`.github/plugin-ci.json`, flat), the install rule does the same, and
`plugin.json`'s `formats` names it, so LogSquirl 26.12 and later read it
while the plugin is enabled; older versions need it imported.

LogSquirl does not match or extract fields from a line longer than
1,000,000 characters (`MaxUiThreadLineLength`), so a line over about 1 MB,
a message with a large payload, gets no columns in the Table View. The
regex covers such lines; the test checks that with a 4 MiB payload.

### Authentication, TLS and connection profiles

`SessionConfig::auth` says how a session logs in (`AuthMethod`: none, user
and password, token, a `.creds` file, an NKey seed file) and
`SessionConfig::tls` how it uses TLS (CA file, client certificate and key,
server name, skipping the verification). `SessionConfig::url` takes
several comma-separated URLs, the seeds of a cluster; each is trimmed and
given to nats.c as its server list. [`src/natsoptions.cpp`](../src/natsoptions.cpp)
turns them into nats.c options, checking each file named first, and
explains a failed connect in words the user can act on ("the server
refused the user name or password", "the server's certificate is not
trusted: set the CA file …"), followed by nats.c's own text. The session
writes that into its `connection to … failed:` marker and reports it, so
the connection's tile shows it (`failed: …`) and a notification says it.

- What each method is (its name in a profile, its label in the dialog,
  whether it takes a user, a secret or a file, what the server refusing it
  means) is one table, `kAuthMethods` in
  [`src/authmethods.h`](../src/authmethods.h); `configureAuth()` in
  natsoptions.cpp sets each on nats.c's options.
- For an NKey seed file nats.c wants the public key too; the session
  derives it from the seed (ed25519 through the linked OpenSSL), in
  [`src/nkey.cpp`](../src/nkey.cpp).
- The TLS settings apply whenever the connection uses TLS: *Use TLS*
  checked, or a `tls://` URL (`usesTls()`); the dialog's TLS fields are
  enabled then, and only then.
- With TLS 1.3 a server that refuses the client certificate closes the
  connection after the handshake without saying why, so that message says
  the server *may* require or refuse a client certificate.
- With TLS off, nats.c still uses TLS when the server requires it, but
  without the dialog's CA; the message then says to switch TLS on.
- Where nats.c's status codes tell the cases apart (refused credentials,
  TLS required or not offered, the connection closed), the explanation
  uses them. The TLS failures are all `NATS_SSL_ERROR`, so those are told
  apart by OpenSSL's text, matched against the pattern lists at the end
  of natsoptions.cpp's anonymous namespace. That text depends on the
  OpenSSL version: after updating OpenSSL, run the `[auth]` tests, which
  fail when a pattern no longer matches.
- A URL may hold credentials (`nats://bob:s3cret@host`). Wherever a URL is
  shown or written (the tile, the host log and notifications, the
  capture file's markers, including nats.c's connected URL) it goes
  through `redactUrls()`, which replaces the password, or a user
  information without one (a token), by `***`.
- The worker clears its copy of the password or token once nats.c's
  options hold it; `NatsSession` keeps its own, which `start()` needs
  again.

The dialog saves each connection as a named **profile**
([`src/profiles.h`](../src/profiles.h)) in `nats.ini`, array `profiles`:
URLs, method, user name, file paths, the TLS settings, and in the group
`capture` the subjects and limits (a profile saved without them gets the
defaults). Never a password or token: those are asked for at Start; a URL
with credentials in it is not saved. `startProblem()` says what a profile
lacks to start; the dialog checks it at Save, `Sessions` again at Start.

The tests (`[auth]`, part of the server tests) are at the main seam,
`auth_test.cpp`: a server started with a fixture configuration, a session
with the right and with a wrong credential or certificate, and the
message it gives, and that a password in a URL is never written;
`plugin_test.cpp` checks the same messages on the tile, and the profiles.

## Building

```sh
cmake -B build -S . -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

Needs CMake 3.28 or later, Qt 6 (Core, Widgets; Network for the tests), a
C and C++17 compiler, Perl and make. Without `-G Ninja` CMake's default
generator works too; with Qt not on the `PATH`, add
`-DCMAKE_PREFIX_PATH=<Qt prefix>`. On Windows: the MSVC environment (`nmake`) and a native Perl
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

In the order the top-level `CMakeLists.txt` includes them, at configure
time:

1. `Dependencies.cmake` sets the pinned versions, URLs and SHA-256s.
2. `OpenSSL.cmake` downloads and verifies the tarball (`FetchContent`),
   runs OpenSSL's `Configure` with Perl, then `make build_libs` and
   `make install_dev` (`nmake` on Windows) into `build/openssl/`. A stamp
   there, keyed on the version, hash, options and C compiler, skips all of
   it the next time. `--openssldir` is `/etc/ssl` on Linux and macOS, the
   system's certificate directory rather than a path of the build machine;
   on Windows, which has no such directory, it is `build/openssl/ssl`.
3. It defines the imported targets and sets `OPENSSL_ROOT_DIR`,
   `OPENSSL_SSL_LIBRARY` and the rest in the cache, then checks with
   `find_package(OpenSSL <version> EXACT)` that this build is what is found.
4. `NatsC.cmake` sets nats.c's `NATS_BUILD_*` options and makes it
   available (`SYSTEM`, `EXCLUDE_FROM_ALL`): its own
   `find_package(OpenSSL)` finds step 3's, and it defines `nats_static`.
   Under UBSan one check, `nonnull-attribute`, is turned off for nats.c
   alone (the comment there says why).
5. The interface target `logsquirl_nats_common` links Qt, `nats_static` and
   `OpenSSL::Crypto` (for the NKey code): the plugin library and the test
   program, which compiles the same sources, both link it.

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
3. Release the plugin: bump `plugin.json` and `project(VERSION)`, and say
   which version it carries now in a `CHANGELOG.md` section.

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

Hidden tests (`[.benchmark]`) run in neither; see
[the benchmark](#dropped-messages-and-the-benchmark).

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
  `replay_test.cpp` with JetStream,
  and through the tab and the dialog in `plugin_test.cpp`, driven against
  `FakeHost`): start a `NatsSession`,
  publish with `nats_test::Publisher`, and assert on the lines of
  `session.filePath()` (`nats_test::readLines`, `nats_test::waitFor` from
  [`tests/sessiontest.h`](../tests/sessiontest.h)). A session is subscribed
  once its first line, the `connected to …` marker, is in the file; from
  then on every message published is captured.
- **The formatter** (`lineformat_test.cpp`): `formatLine()` is a pure
  function from a message to one line.
- **The Log Format** (`logformat_test.cpp`) is tested at the same seam: it
  reads the formatter's lines with the regex and timestamp format of
  [`formats/nats_log.json`](../formats/nats_log.json), as LogSquirl does,
  and checks the columns. A change of the line format that the definition
  does not follow fails it.

### Dropped messages and the benchmark

A capture never loses a message silently: nats.c's slow-consumer drops
(beyond the subscription's pending limits) and the drops of the session's
own queue (beyond `SessionConfig::queueLimit`) are each counted in one
marker line, `$NATS-MONITOR - <n> dropped=<count> (slow consumer)` or
`(queue full)`, and in `NatsSession::droppedCount()`. The stress tests
(`[drops]`, part of the server tests) flood a session with small limits
and check that the `dropped=` counts add up to the messages published but
not written.

Whether v1 keeps up with its target, **50k msgs/s at 200 B** without drops,
is a benchmark, not run by ctest or CI (its tag is hidden). Run it by hand
on a Release build, on an otherwise idle machine:

```sh
build/tests/logsquirl_nats_tests "[benchmark]"
```

It publishes at that rate for 10 seconds with the default limits, requires
every message written and no `dropped=` marker, and prints the rate
reached (a Catch2 warning, also when it passes).

### Test fixtures: certificates and NATS credentials

They are committed, not generated per run, so the tests need no `openssl`
program (the Windows runners have none) and no `nsc`, and behave the same
everywhere. None of the keys protects anything; never use them outside the
tests.

- `tests/certs/`, made by `tests/certs/generate.sh` (openssl 1.1.1 or
  later), valid for 100 years: a test CA, a server certificate for
  `localhost` / `127.0.0.1` and a client certificate signed by it, and a
  client certificate of another CA, which the servers of the tests do not
  trust. The CAs' keys were thrown away.
- `tests/auth/`, made by `tests/auth/generate.py` (Python 3.8+ and openssl
  on the PATH, no nsc): `operator.conf`, a nats-server configuration in
  operator mode with the operator's JWT and the account JWTs preloaded in
  a memory resolver; `user.creds`, a user of that account; `other.creds`,
  a user of an account the server does not know; `nkey.seed` and
  `nkey.pub`, an NKey user; `other.seed`, an NKey the server does not
  know. The script signs the JWTs itself with ed25519 keys made by openssl;
  the JWTs never expire. nats-server checks every signature when the tests
  run, so a mistake in the script fails them.

To regenerate, run the script and commit what changed; the tests read the
files as they are, so nothing else changes.

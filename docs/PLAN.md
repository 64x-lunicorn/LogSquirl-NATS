# LogSquirl-NATS — Plan

A LogSquirl plugin that subscribes to NATS subjects and writes every message as
one line to a growing file that LogSquirl follows, so live traffic can be
searched, filtered and highlighted like any log — also at high message rates.

Status: plan only, no code yet. Each section under [Tickets](#tickets) is meant
to become one GitHub issue (see `docs/agents/issue-tracker.md` once the repo is
synced from LogSquirl-Plugin-CI).

## Decisions

| Question | Decision | Why |
|---|---|---|
| NATS client | **nats.c** (official C client, Apache-2.0), built statically via CMake `FetchContent` at a pinned tag | Full protocol: headers, NKey/creds (ed25519), TLS, JetStream; no user-installed CLI. Apache-2.0 is compatible with GPL-3.0-or-later. |
| How messages reach LogSquirl | **Plugin type `ui`, temp file + `open_file(path, follow=1)`**, as LogSquirl-Serial does | Works with today's LogSquirl releases; LogSquirl's indexing of large growing files does the heavy lifting; the capture survives Stop. The DataSource `push_line` API (LogSquirl 26.12) is unreleased and untested at high rates. |
| Scope of v1 | Core NATS subscribe; JetStream replay in the release after | Live capture first; replay builds on the same writer and line format. |

## Architecture

Modelled on LogSquirl-Serial (`src/serialprocess.*`, `src/portwidget.*`,
`src/tempdirs.*`, `guarded()` in `src/plugin.cpp`):

```
Tools ▸ Plugins ▸ NATS Monitor…        (register_menu_action)
        │
        ▼
ConnectionDialog (QDialog)            server URL(s), auth, subjects, queue group,
        │                             size limit; Start / Stop per session
        ▼
NatsSession                           one natsConnection, N natsSubscriptions
  ├─ nats.c delivery thread(s)        message callback: format line, enqueue — never block
  ├─ LineQueue (bounded, lock-free    or mutex + condvar)
  └─ Writer thread                    drains queue in batches → QFile, flush per batch,
                                      rotation at size limit, drop counter
        │
        ▼
temp file  logsquirl-nats-<pid>-XXXXXX/nats_<name>.log  → open_file(follow=1)
```

- **Threading.** nats.c delivers messages on its own threads. The callback only
  formats the line and enqueues it; the writer thread owns the file. Nothing
  touches the UI thread except status updates via queued signals. Every host
  callback and entry point goes through `guarded()`.
- **Backpressure.** NATS core drops messages for slow consumers. Set generous
  pending limits (`natsSubscription_SetPendingLimits`), react to the async
  error `NATS_SLOW_CONSUMER`, read `natsSubscription_GetDropped`, and write a
  marker line (`… NATS-MONITOR dropped=1234 since last marker`) so gaps are
  visible in the log itself, plus a counter in the dialog/status widget.
  If the plugin's own queue overflows, it drops too and counts the same way.
- **Limits.** Size limit with rotation, as Serial does (#33 there): default
  e.g. 256 MiB, 0 = unlimited. Rotation opens the next file in a new tab.
- **Temp dirs.** Same lifecycle as Serial: kept until LogSquirl quits, removed
  by PID on quit and stale ones at `init()`.

## Line format

One message per line, fields separated by spaces; NATS subjects and reply
subjects contain no spaces, so the fixed fields parse with a simple regex:

```
<UTC timestamp>                 <subject>          <reply|->  <size> <body>
2026-11-11 10:00:00.123456Z orders.eu.created _INBOX.ab12 57 {"id":1,"total":9.5}
2026-11-11 10:00:00.124001Z sensors.t1        -           4  [Nats-Msg-Id=42; Trace=abc] 21.5
```

- **Timestamp**: receive time (nats.c gives no publish time for core NATS;
  JetStream replay uses the stream's stored timestamp instead).
- **Headers** (optional): `[k=v; k=v]` before the payload, values escaped.
- **Payload**: UTF-8 text as is, with `\n`, `\r`, `\\` escaped so one message
  stays one line; non-UTF-8 / binary as `hex:` + hex (cut at a configurable
  length, size field still the real size). JSON stays on one line as received.
- **Log Format definition** `formats/nats_log.json` (lnav format, shipped via
  `package_files` like tcpdump's) gives the Table View columns *timestamp,
  subject, reply, size, body*, with subject as identifier.

## Tickets

### Milestone 0.1.0 — live capture

1. **Scaffold the repository.** `sync-plugin.sh` from LogSquirl-Plugin-CI,
   `plugin.json` (`io.github.logsquirl.nats`, type `ui`, library
   `logsquirl_nats`), CMake, nats.c via FetchContent with TLS, an empty plugin
   that links nats.c and passes CI on Linux, macOS arm64 and Windows.
   *Do this first: TLS/OpenSSL on three platforms is the biggest risk (see
   open questions).*
2. **Connect and subscribe to one subject into a followed file.** Dialog with
   server URL and subject (wildcards `*`, `>`), Start/Stop, writer thread,
   temp file + `open_file(follow=1)`, tab keeps the capture after Stop.
3. **Line format and Log Format definition.** Format above, escaping, binary
   as hex, headers; `formats/nats_log.json` with tests against sample lines.
4. **Slow consumers and dropped messages are visible.** Pending limits, async
   error handler, marker lines, counter in the dialog.
5. **Size limit and rotation.** As in Serial, default and 0 = unlimited.
6. **Authentication and TLS.** User/password, token, `.creds` file, NKey seed
   file, TLS with CA / client cert / key, "skip verify" off by default.
   Profiles saved under `get_config_dir()`: paths and settings only, never
   passwords or tokens in plain text.
7. **Several subjects and sessions.** Multiple subjects per session (one file),
   optional queue group, several sessions side by side, status widget with
   msgs/s and dropped count per session.
8. **README, DEVELOPER_GUIDE, CHANGELOG, icon.**

### Milestone 0.2.0 — JetStream

9. **Replay a stream.** Pick a stream (`$JS.API.STREAM.NAMES`), subject
   filter, start at: all / last N / sequence / time; ordered ephemeral
   consumer; stream timestamp and sequence in the line; then optionally keep
   following live.
10. **Inspect without consuming.** Ensure replay never acks into or creates a
    durable consumer on the server (ephemeral, `AckNone`).

### Backlog

- Protobuf decoding with a user-supplied descriptor set.
- KV / Object Store views.
- Publish a message from the dialog (request/reply testing).
- DataSource `push_line` variant once LogSquirl 26.12 is released and its
  throughput is measured.

## Open questions

- **TLS backend.** nats.c needs OpenSSL for TLS. LogSquirl ships no OpenSSL
  (Qt uses SecureTransport / Schannel on macOS / Windows), and
  `bundle_qt_frameworks` only covers Qt libraries. Options: link OpenSSL
  statically into `logsquirl_nats` (vcpkg / Homebrew static libs in CI), or
  extend LogSquirl-Plugin-CI with a way to bundle non-Qt libraries. Decide in
  ticket 1.
- **NKey signing.** Check whether the pinned nats.c needs libsodium or uses its
  built-in ed25519 (prefer built-in: one dependency less).
- **Throughput target.** Which message rate must v1 sustain without drops on a
  typical laptop (e.g. 50k msgs/s at 200 B)? Becomes a test/benchmark in
  ticket 4.
- **Repository.** `64x-lunicorn/LogSquirl-NATS` on GitHub, add to the plugin
  table and `plugin-drift.yml` in LogSquirl-Plugin-CI, and to LogSquirl's
  `plugins.json` at the first release.
- **Before writing the plugin**, try it without one: `nats sub '>' --headers > nats.log`
  opened with Follow in LogSquirl, to confirm the subject column, replay and
  the connection dialog are what is missing.

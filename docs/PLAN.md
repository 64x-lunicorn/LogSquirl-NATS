# LogSquirl-NATS — Plan

A LogSquirl plugin that subscribes to NATS subjects and writes every message as
one line to a growing file that LogSquirl follows, so live traffic can be
searched, filtered and highlighted like any log — also at high message rates.

Status: plan only, no code yet. Tickets: see [Tickets](#tickets).

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

The tickets live as GitHub issues, planned by milestone; they are the source of
truth, this plan only gives the overview:
[milestone 0.1.0](https://github.com/64x-lunicorn/LogSquirl-NATS/milestone/1)
(live capture: #1 scaffold, #2 subscribe and line format, #3 Log Format,
#4 drops, #5 rotation, #6 auth and TLS, #7 several subjects and sessions,
#8 docs) and
[milestone 0.2.0](https://github.com/64x-lunicorn/LogSquirl-NATS/milestone/2)
(JetStream: #9 replay, #10 consumer cleanup and permissions).

## Test seams

- **Main seam: a capture session against a real `nats-server`**, which the test
  build downloads at a pinned, checksum-verified version (#1): publish, then
  assert on the lines in the output file. Prior art: LogSquirl-Serial's
  `serialprocess_test`.
- **Second seam: the formatter**, a pure function from message to line.

### Backlog

- Protobuf decoding with a user-supplied descriptor set.
- KV / Object Store views.
- Publish a message from the dialog (request/reply testing).
- DataSource `push_line` variant once LogSquirl 26.12 is released and its
  throughput is measured.

## Open questions

- ~~**TLS backend.**~~ Decided in #1: OpenSSL 3.x LTS built statically by the
  plugin's own CMake and linked into `logsquirl_nats`; see
  [DEVELOPER_GUIDE.md](DEVELOPER_GUIDE.md).
- ~~**NKey signing.**~~ nats.c's built-in ed25519, no libsodium (#1).
- **Throughput target.** Which message rate must v1 sustain without drops on a
  typical laptop (e.g. 50k msgs/s at 200 B)? Asked in #4.
- **Repository.** `64x-lunicorn/LogSquirl-NATS` on GitHub, add to the plugin
  table and `plugin-drift.yml` in LogSquirl-Plugin-CI, and to LogSquirl's
  `plugins.json` at the first release.
- **Before writing the plugin**, try it without one: `nats sub '>' --headers > nats.log`
  opened with Follow in LogSquirl, to confirm the subject column, replay and
  the connection dialog are what is missing.

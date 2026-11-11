# LogSquirl-NATS — Plan

A LogSquirl plugin that subscribes to NATS subjects and writes every message as
one line to a growing file that LogSquirl follows, so live traffic can be
searched, filtered and highlighted like any log — also at high message rates.

Status: 0.1.0, live capture (#1–#8), is implemented and not yet released;
JetStream replay (0.2.0) is next. How the code works:
[DEVELOPER_GUIDE.md](DEVELOPER_GUIDE.md#architecture). Tickets: see
[Tickets](#tickets).

## Decisions

| Question | Decision | Why |
|---|---|---|
| NATS client | **nats.c** (official C client, Apache-2.0), built statically via CMake `FetchContent` at a pinned tag | Full protocol: headers, NKey/creds (ed25519), TLS, JetStream; no user-installed CLI. Apache-2.0 is compatible with GPL-3.0-or-later. |
| How messages reach LogSquirl | **Plugin type `ui`, temp file + `open_file(path, follow=1)`**, as LogSquirl-Serial does | Works with today's LogSquirl releases; LogSquirl's indexing of large growing files does the heavy lifting; the capture survives Stop. The DataSource `push_line` API (LogSquirl 26.12) is unreleased and untested at high rates. |
| Scope of v1 | Core NATS subscribe; JetStream replay in the release after | Live capture first; replay builds on the same writer and line format. |
| TLS backend | **OpenSSL 3.x LTS, built by the plugin's CMake, linked in statically** | No change to the shared CI, no extra library in the package, same on every platform; [ADR 1](adr/0001-static-openssl-built-by-the-plugin.md). |
| Saved connections | **Profiles in `nats.ini`** (URLs, auth method, user name, file paths, TLS settings); **passwords and tokens never saved**, entered at Start (#6) | Nothing secret in a plain-text file; a `.creds` file is the way to keep credentials without typing them. A keychain is not needed for v1. |
| Overlapping subjects in a session | **One line per subscription**, no deduplication; the dialog warns and, when one subject covers the other, suggests dropping the narrower one (#7) | nats.c delivers a matching message once per subscription (checked by a test against the pinned nats.c); deduplicating would hide what the server actually delivers. |
| Queue groups | **Off by default**, per subject (#7) | A session in a queue group takes messages away from the group's other members. |

## Architecture

Modelled on LogSquirl-Serial (`src/serialprocess.*`, `src/portwidget.*`,
`src/tempdirs.*`, `guarded()` in `src/plugin.cpp`):

```
Tools ▸ Plugins ▸ NATS Monitor…        (register_menu_action)
        │
        ▼
ConnectionDialog (QDialog)            server URL(s), auth, subjects (each with an
        │                             optional queue group), size limit; Start adds
        │                             a session, Stop / Stop all; running sessions listed
        │ ──▶ StatusWidget            status bar (register_status_widget): sessions,
        │                             msgs/s, dropped
        ▼  one per Start, side by side
NatsSession                           one natsConnection, N natsSubscriptions
  ├─ nats.c delivery thread(s)        message callback: format line, enqueue — never block
  ├─ queue (bounded, mutex + condvar)
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
  marker line (`… $NATS-MONITOR - 28 dropped=1234 (slow consumer)`, the drops
  since the last marker) so gaps are visible in the log itself, plus a
  counter in the status widget (#7).
  If the plugin's own queue overflows, it drops too and counts the same way.
  nats.c reports how many it dropped, not where: a slow-consumer marker may
  land up to the pending-message limit earlier in the file than the actual
  gap, since the messages nats.c still held when it dropped are written
  after the marker. On stop each subscription is drained, so nothing nats.c
  holds is discarded uncounted.
- **Several subjects, several sessions** (#7). A session subscribes to one
  or more subjects, each optionally in a queue group, and writes all of them
  into its one file. Overlapping subjects (`orders.>` and `orders.eu.*`)
  give one line per subscription, as the server delivers them; the dialog
  warns. Each Start adds a session beside those running, with its own
  connection, file and tab, as Serial's sessions per port; shutdown stops
  them all. The status widget shows how many run, the msgs/s they receive
  and how many the running sessions dropped.
- **Limits.** Size limit with rotation, as Serial does (#33 there): set in
  the dialog in MiB, default 256 MiB, 0 = no limit, remembered between
  sessions (`nats.ini` in the plugin's configuration directory). Before a
  line that would take the file over the limit, the writer goes on in the
  next file, so a message line is never split: `nats_<name>.log`,
  `nats_<name>.2.log`, … Each opens in a new followed tab and is logged to
  the host log (not notified, as in Serial). A line longer
  than the limit is written whole, alone in its file. A rotation that fails
  (the next file cannot be created) is reported once, and the capture goes
  on in the same file until it has grown by the limit again.
- **Temp dirs.** Same lifecycle as Serial: kept until LogSquirl quits, removed
  by PID on quit and stale ones at `init()`.

## Line format

One message per line, fields separated by one space; NATS subjects and reply
subjects contain no spaces, so the fixed fields parse with a simple regex:

```
<UTC timestamp>             <subject>         <reply|-> <size> [<headers>] <payload>
2026-11-11 10:00:00.123456Z orders.eu.created _INBOX.ab12 20 {"id":1,"total":9.5}
2026-11-11 10:00:00.124001Z sensors.t1 - 4 [Nats-Msg-Id=42; Trace=abc] 21.5
```

- **Timestamp**: receive time in UTC, with microseconds (nats.c gives no
  publish time for core NATS; JetStream replay uses the stream's stored
  timestamp instead).
- **Size**: the payload's real size in bytes, also when it is cut.
- **Headers** (optional): `[k=v; k=v]` before the payload, sorted by key (a
  key with several values once per value). `;`, `]`, `\` and line breaks in
  keys and values are escaped by a backslash, and `=` in keys too.
- **Payload**: UTF-8 text as is, with `\n`, `\r`, `\\` escaped and the other
  C0 control characters but tab as `\xNN`, so one message stays one line; a
  leading `[` or `hex:` is escaped as well (`\x5B`, `\x68ex:`), so it is not
  taken for headers or a binary payload. Non-UTF-8 / binary as `hex:` + hex,
  cut at a length set in the dialog (default 256 bytes). JSON stays on one
  line as received.
- **Markers**: connection events with the subject `$NATS-MONITOR`, which no
  client can publish to, in the same format.
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
  [ADR 1](adr/0001-static-openssl-built-by-the-plugin.md) and
  [DEVELOPER_GUIDE.md](DEVELOPER_GUIDE.md).
- ~~**NKey signing.**~~ nats.c's built-in ed25519, no libsodium (#1).
- ~~**Throughput target.**~~ Decided in #4: v1 sustains **50k msgs/s at
  200 B payload** without drops on a typical laptop, with the default pending
  limits of 1,000,000 messages / 512 MiB. The benchmark that checks it is
  opt-in and local (hidden Catch2 tag `[.benchmark]`, see
  [DEVELOPER_GUIDE.md](DEVELOPER_GUIDE.md)); CI runs only the deterministic
  stress tests.
- **Repository.** `64x-lunicorn/LogSquirl-NATS` on GitHub, add to the plugin
  table and `plugin-drift.yml` in LogSquirl-Plugin-CI, and to LogSquirl's
  `plugins.json` at the first release.
- **Before writing the plugin**, try it without one: `nats sub '>' --headers > nats.log`
  opened with Follow in LogSquirl, to confirm the subject column, replay and
  the connection dialog are what is missing.

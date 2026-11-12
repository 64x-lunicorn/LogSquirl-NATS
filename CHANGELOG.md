# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- **Replay a JetStream stream into a tab.** A connection can replay a
  stream instead of capturing subjects: chosen from the server's streams
  (*List*), optionally only one subject, from its start, the last N
  messages, a sequence or a time, and optionally following live once
  caught up. It reads through an ordered ephemeral consumer that never
  acknowledges, so the stream is left as it is. A replayed line has the
  time the stream stored the message and its stream sequence (`#1042`,
  before the size); the Log Format gets the column *seq*. A replay that
  does not follow live ends with `replay complete`, and its tile says so.
  A stream of any size is written whole, and Stop ends a replay at any
  point (#9).
- **A replay leaves no consumer behind.** Stop, a completed replay,
  unloading the plugin and quitting LogSquirl delete the replay's
  consumer; after a crash or a connection lost for good, the server
  deletes it 5 seconds after its reader went away (inactive threshold). A
  stream with work-queue retention is not replayed, saying why; one with
  interest retention is replayed after a warning line. The README lists
  the least NATS permissions for live capture and for replay, which
  cannot change the stream's messages (#10).

## [0.1.0]

### Added
- Initial release of the LogSquirl NATS Monitor plugin: live capture of
  core NATS subjects into a followed LogSquirl tab.
- **Subscribe and follow.** Start connects, subscribes and opens the
  capture in a new tab in follow mode. Subjects take the `*` and `>`
  wildcards. The capture stays in the tab after Stop, until LogSquirl
  quits (#2).
- **One message, one line.** UTC receive time with microseconds, subject,
  reply subject (`-` for none), payload size, the headers as `[k=v; k=v]`,
  and the payload escaped to one line; a payload that is not UTF-8 is
  written as `hex:`, cut at a length set in the dialog (256 bytes by
  default). Connection events are marker lines with the subject
  `$NATS-MONITOR` (#2).
- **Columns in the Table View.** The Log Format `nats_log.json` gives the
  columns *timestamp, subject, reply, size, body*, with the subject as
  identifier. LogSquirl 26.12 and later read it while the plugin is
  enabled; older versions need it imported (#3).
- **No message lost silently.** Messages nats.c drops for a slow consumer
  and those the plugin's own queue drops are counted in a
  `dropped=<n> (slow consumer)` or `(queue full)` marker line where the gap
  is. Pending limits are set in the dialog (1,000,000 messages and 512 MiB
  by default), and stopping drains each subscription first (#4).
- **A capture's file has a size limit.** When the next line would take the
  file over it (256 MiB unless you choose another, *No limit* turns it
  off), the capture goes on in a new file, opened in a new followed tab and
  noted in LogSquirl's log; a message line is never split. A new
  connection proposes the last limit saved (#5).
- **Authentication and TLS.** User and password, token, a `.creds` file or
  an NKey seed file; TLS with a CA file, a client certificate and key, a
  server name, and (marked insecure) skipping the verification; several
  comma-separated URLs as the seeds of a cluster. A connection that fails
  says why in words you can act on. Credentials in a URL are never saved,
  shown or written (#6).
- **Saved connections.** A connection's settings (server, authentication,
  TLS, subjects, limits) are saved by name in a dialog. A password or
  token is never saved: it is asked for at Start (#6, #20).
- **Several subjects, several sessions.** A session subscribes to several
  subjects, one a line, each optionally in a queue group, all into one
  file; the dialog warns when subjects overlap. Several connections run
  side by side, each with its own connection, file and tab (#7).
- **A sidebar tab, NATS, with a tile per saved connection.** Each tile
  shows the server, how it logs in, TLS and the subjects, with Start /
  Stop, Edit and Delete; while it runs, its state, msgs/s, received and
  dropped counts, the server it is connected to and its file; after it
  failed, why. *New connection…* opens the dialog, and *Stop all* stops
  every tile (#20).
- nats.c and OpenSSL 3.5 (LTS) linked in statically: the package carries no
  other library and uses no system OpenSSL (#1).
- Unit tests with Catch2 v2, most of them against a real `nats-server` the
  test build downloads; CI on Linux, macOS and Windows (#1).

[Unreleased]: https://github.com/64x-lunicorn/LogSquirl-NATS/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/64x-lunicorn/LogSquirl-NATS/releases/tag/v0.1.0

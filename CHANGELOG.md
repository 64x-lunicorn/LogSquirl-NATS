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

### Changed
- **The connection tile is a card.** A rounded frame, in the accent colour
  while the connection runs; a dot before the name for its state (grey
  stopped, amber connecting, green running, red failed); the subjects
  labelled; the details muted below a thin line. Delete is hidden, not
  only disabled, while the connection runs. Light and dark themes both
  work (#23).
- **Readable file names.** A wildcard in a subject is spelled out in the
  file's name and so in the tab's title: `>` gives `nats_all.log`,
  `orders.>` `nats_orders.all.log` and `billing.*` `nats_billing.any.log`,
  where they were `nats__.log`, `nats_orders._.log` and
  `nats_billing._.log` (#23).
- **A failed delete says why.** When a connection cannot be deleted from
  its tile, a warning says why (it is capturing or replaying, it is no
  longer saved, or nats.ini could not be written) instead of a
  notification that it could not be deleted (#31, #41).
- **The texts say session, capture and replay as the glossary does.** A
  tooltip, a dialog message or a log line says *session* where it means a
  capture or a replay, and *capture* or *replay* only for that kind: Stop
  all stops every session, and a running replay cannot be deleted because
  it is replaying (#41).

### Fixed
- **A write error ends the session as failed, and its tile says why.**
  When the capture's file can no longer be written (disk full, the file
  removed, no permission), the session ends as a stop does, but failed:
  the tile's dot turns red and its detail names the write error and the
  file, and *received* stops growing. It went on as running before,
  counting lines that were never written. The connection can be started
  again once the cause is fixed (#39).
- **A replay start that cannot be read is refused.** A stream sequence
  above the largest one (18446744073709551615) or left empty is not saved,
  where it was replayed from sequence 1; a sequence or time in nats.ini that
  is not a number, or a time before 1970-01-01 00:00:01 or after
  2262-04-11 23:47:16 UTC, keeps the connection from starting, and its tile
  says why (#38). Saving or deleting another connection keeps such a
  sequence or time as it was, where it became sequence 1 or the time 0
  (#44).
- **A number of last messages that cannot be read is refused.** A
  `lastMessages` in nats.ini that is not a number, is 0 or is above
  18446744073709551615 keeps the connection from starting, and its tile
  says why, where the last 100 messages were replayed; one not saved still
  replays the last 100. The dialog's *Last messages* field is a text field
  like the sequence's: it takes any number up to 18446744073709551615, where
  it stopped at 2147483647, and shows a saved number that cannot be read as
  it is, so Save says why (#42).

### Security
- **Secrets are wiped from memory once nats.c has them.** A password or
  token, and an NKey seed file's contents and its decoded seed, are
  overwritten as soon as nats.c's connection options hold what they need;
  a running capture keeps no copy of the password or token (#36).
- **The secret's copies on the way to a session are wiped too.** The
  password or token asked for at Start, and the copies made of it on the
  way to the session, are overwritten once the session has its own; the
  one asked for to list the streams, once the listing has connected (#43).

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

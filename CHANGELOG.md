# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [0.1.0]

### Added

- Initial release of the LogSquirl NATS Monitor plugin: live capture of
  core NATS subjects into a followed LogSquirl tab.
- **Subscribe and follow.** *Plugins → NATS Monitor…* opens a dialog; Start
  connects, subscribes and opens the capture in a new tab in follow mode.
  Subjects take the `*` and `>` wildcards. The capture stays in the tab
  after Stop, until LogSquirl quits (#2).
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
  noted in LogSquirl's log; a message line is never split. The limit is
  remembered between sessions (#5).
- **Authentication and TLS.** User and password, token, a `.creds` file or
  an NKey seed file; TLS with a CA file, a client certificate and key, a
  server name, and (marked insecure) skipping the verification; several
  comma-separated URLs as the seeds of a cluster. A connection that fails
  says why in words you can act on. Credentials in a URL are never shown or
  written (#6).
- **Connection profiles.** The connection settings are saved by name and
  loaded from a list. A password or token is never saved: it is asked for
  at Start (#6).
- **Several subjects, several sessions.** A session subscribes to several
  subjects, one a line, each optionally in a queue group, all into one
  file; the dialog warns when subjects overlap. Each Start adds a session
  with its own connection, file and tab; Stop stops the one selected,
  Stop all every one (#7).
- **Status bar widget.** `NATS: 2 sessions · 1,234 msgs/s · 5 dropped`, or
  `NATS: no capture` (#7).
- nats.c and OpenSSL 3.5 (LTS) linked in statically: the package carries no
  other library and uses no system OpenSSL (#1).
- Unit tests with Catch2 v2, most of them against a real `nats-server` the
  test build downloads; CI on Linux, macOS and Windows (#1).

[Unreleased]: https://github.com/64x-lunicorn/LogSquirl-NATS/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/64x-lunicorn/LogSquirl-NATS/releases/tag/v0.1.0

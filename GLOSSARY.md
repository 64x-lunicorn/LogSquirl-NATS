# LogSquirl NATS Monitor

A LogSquirl plugin that captures NATS messages, live or replayed from a
JetStream stream, into files LogSquirl follows as tabs.

## Language

### Connections

**Connection**:
A connection's settings saved under a name: its server, how it logs in, TLS,
and the subjects it captures or the stream it replays; a tile in the sidebar.
Never a password or token.
_Avoid_: Session (a session is a connection running)

**Profile**:
A connection as saved in nats.ini (`ConnectionProfile`).

**Profile store**:
The one owner of nats.ini, opened on its path: lists, finds, saves and
deletes the profiles, saying why when it cannot, and remembers the defaults a
new connection proposes, such as the size limit saved last (`ProfileStore`).
The dialog and the sidebar both go through it.
_Avoid_: Settings, config file

**Start problem**:
Why a connection cannot be started, in words for the user, or none: the one
check of its rules, made by Sessions at Start and by the session again on
its config (`startProblem()`). A replay's rules in it are the replay
module's (`replayProblem()`).
_Avoid_: Validation error

**Save problem**:
Why a connection cannot be saved, or none: no name, Replay checked without a
stream, credentials in a URL, then its start problem (`saveProblem()`). The
profile store refuses to save a profile with one.

### Capture

**Capture sink**:
Everything between a received message and a capture's files: the bounded
queue, the accounting of drops, the back-pressure of a replay, the writer
thread, rotation at the size limit and the end of the capture
(`CaptureSink`). It takes message and marker lines from any thread and
reports what it wrote; it knows nothing of NATS. The session feeds it.
_Avoid_: Writer (the sink's thread), queue (a part of it)

**Marker**:
A line the plugin writes itself, on the subject `$NATS-MONITOR`: a
connection event, a count of dropped messages, the end of the capture.

**Transition**:
A step of a session's life as the session reports it, once, typed:
connecting, running at a server, reconnecting, failed with a reason,
completed, stopped (`Transition`), with a detail text for the user. Nothing
reads a state out of that text.
_Avoid_: Status change, event (the capture sink's reports)

**Connection status**:
What Sessions, its one owner, keeps of a connection: the state its
session's transitions led to, the detail, the server, the file, the counts
and the msgs/s it samples on its clock (`ConnectionStatus`, read as a
`SessionStatus`). The tile only shows it.
_Avoid_: Tile state (the tile's dot, mapped from it)

### Host

**Host adapter**:
What Sessions needs of LogSquirl, given to it: open a file in a followed
tab, show a notification, write to the host log (`HostAdapter`). The
plugin's calls the plugin API; a test's records in memory what was opened,
notified and logged, with no plugin initialised.
_Avoid_: Host API (the C function table the plugin's adapter calls)

### Replay

**Replay**:
A session that writes what a JetStream stream holds, from a start position,
instead of subscribing to subjects; it takes nothing away from the stream.
_Avoid_: Playback, import

**Start position**:
Where in its stream a replay begins, as the user chooses it: all, the last N
messages, a stream sequence, or a time (`ReplayStart`).
_Avoid_: Offset

**Stream state**:
What the server tells of a stream before a replay subscribes: its retention,
first and last sequence and message count (`StreamState`).

**Replay plan**:
The decision, made from a replay's config and its stream state before it
subscribes, whether it is refused or warned of, where its consumer starts,
what it skips, and whether it is caught up at once (`ReplayPlan`).

**Replay step**:
The rule applied to each message a replay's consumer delivers: write or skip
it, and whether the replay has caught up with it (`ReplayStep`).

**Caught up**:
A replay has written the last message its stream held: it is then complete,
or it follows live.
_Avoid_: Done, finished (how a session ends is its last transition: completed,
failed or stopped)

**Follow live**:
Going on, once caught up, with what the stream stores from then on.

# LogSquirl NATS Monitor

A LogSquirl plugin that captures NATS messages, live or replayed from a
JetStream stream, into files LogSquirl follows as tabs.

## Language

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
_Avoid_: Done, finished (a finished session is one that failed)

**Follow live**:
Going on, once caught up, with what the stream stores from then on.

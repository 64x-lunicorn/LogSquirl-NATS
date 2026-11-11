# NATS Monitor

**A [LogSquirl](https://github.com/64x-lunicorn/LogSquirl) plugin that streams
NATS messages into a live, searchable tab.**

Subscribe to subjects (with `*` and `>` wildcards), and every message lands as
one line — timestamp, subject, reply subject, size, headers, payload — in a
file LogSquirl follows, so its search, filters and highlighters work on live
traffic, also at high message rates. JetStream replay is planned next.

> **Status: planning.** No release yet. See [docs/PLAN.md](docs/PLAN.md) and
> the [issues](https://github.com/64x-lunicorn/LogSquirl-NATS/issues).

## License

GPL-3.0-or-later, see [LICENSE](LICENSE).

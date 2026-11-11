- Plan and architecture: [docs/PLAN.md](docs/PLAN.md); the reference plugin to follow is LogSquirl-Serial.
- The workflows, `.clang-format`, `docs/agents/issue-tracker.md` and `include/logsquirl_plugin_api.h` are LogSquirl-Plugin-CI's (`scripts/sync-plugin.sh`): change them there.
- Issues and specs: [docs/agents/issue-tracker.md](docs/agents/issue-tracker.md)

## Agent skills

### Issue tracker

Issues live in GitHub Issues on `64x-lunicorn/LogSquirl-NATS`, managed with the `gh` CLI, planned by milestone. See `docs/agents/issue-tracker.md`.

### Triage labels

The five canonical triage roles, each label string equal to its name. See `docs/agents/triage-labels.md`.

### Domain docs

Single-context: `GLOSSARY.md` at the repo root, ADRs under `docs/adr/`. See `docs/agents/domain.md`.

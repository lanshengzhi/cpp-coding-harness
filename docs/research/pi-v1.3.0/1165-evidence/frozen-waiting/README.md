# Frozen waiting probe (preserved reproducible evidence, issue #1165)

Source: `cch-1088-frozen-waiting.ts`, copied from the ephemeral
`/tmp/cch-1088-frozen-waiting.ts` before cleanup. It runs against the frozen
pi-v1.3.0 checkout `71023aec1546d6d677a65669c5665c0927ce0dd0` at
`/tmp/cch-1080-pi-frozen` (ephemeral path; re-pin the checkout by hash to
re-run elsewhere).

What it proves: a phase that commits `waiting` (`on: []`, `policy:
'allSettled'`) and remains active (awaits the abort signal) is observed by
frozen `harness.inspect()` as `record.state.status == "waiting"` with derived
`state.kind == "running"` — running is not derived from committed status
alone.

Re-run: `bun cch-1088-frozen-waiting.ts` (paths inside the probe point at the
frozen checkout; adjust if it moves).

- `cch-1088-frozen-waiting-{1,2,3}.log` — the original three recorded runs
  (copied from `/tmp/cch-1088-frozen-waiting-{1,2,3}.log`).
- `cch-1165-frozen-waiting-rerun-{1,2,3}.log` — three fresh confirmation runs
  on 2026-10-28 against the same frozen checkout, identical output, exit 0.

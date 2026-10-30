# #1099 deterministic Remote replay

## Scope

This is the delivery record for #1099 under #1044 and
[ADR 0075](../../adr/0075-align-selected-owner-packages-and-linux-capabilities-with-pi-v1-3-0.md).
The named baseline is **pi-v1.3.0 / 71023aec1546d6d677a65669c5665c0927ce0dd0**,
Linux x86-64 with glibc. It records deterministic `start`/replay over the real
reorderable Remote exchange; durable journal and task continuation stay with
#1171, and application store/resume stays with #1119.

## Delivered behavior

`cch::codemode::RemoteCodemodeSandbox::start` now mirrors
`CodemodeSandbox::start` over a remote: the caller fixes the start time, seed,
tools, globals, store and expected fingerprint, answers each call through
`RemoteCodemodeSandbox::Run::deliver`, and reads the outcome through
`Run::result`. `serve_codemode_remote` routes a request carrying `started_at`
to the deterministic driver, which owns the virtual clock, seeded randomness,
fingerprint and seq-ordered delivery.

The Linux host posts each reported message as its own exchange and does not wait
for one answer before reporting the next call, so several calls are outstanding
at once. That is what makes the transport reorderable: an answer delivered as
result 2 before result 1 is queued and applied in result order. Refs are
validated synchronously and mirror the local run's messages: an unknown call is
`Call 99 is not waiting for a result`, a reused number is
`Result number 1 is not a positive integer used once`, and a non-finite clock,
non-object rejection properties or non-JSON value are refused without settling
the run.

Abort, close and a remote that returns before the script settled keep their
existing taxonomy. Answers still waiting when the run settles are answered with
`stop` so no exchange is left pending. A run whose input is missing a result
stays unfinished, exactly as frozen pi does; the replay caller that knows its
input is exhausted must cancel or report the gap rather than treat ordinary
completion as replay success.

## Validation

- Focused: `ctest --preset vcpkg -LE architecture -L codemode` →
  **113/113 passed**. The `remote` label holds **14** cases; five are new
  `[issue1099]` cases covering the normal deterministic exchange, reversed
  answers applied by result number, the duplicate/unknown refusals, a missing
  result that stays pending until the caller cancels, and the seed/fingerprint
  replay refusals.
- Architecture: `ctest --preset vcpkg -L architecture` → **43/43 passed**,
  including `cch_warning_gate`, after reconfiguring for the new include set.
- Full: `cmake --build --preset vcpkg && ctest --preset vcpkg` →
  **4,025 selected, 4,025 passed** before the review fix. After it, repeated
  `-j 2` runs on the same host reported only pre-existing load-sensitive cases
  (durable generation workflow, TUI terminal process, theme query timing,
  projection cost, one `quarantine`-labelled cost bound); each failed case
  passed when run alone, and the host load average was 5–19 from other work.
  No bound was retuned and no test was quarantined.
- Formatting: `scripts/format-check.sh origin/main` → clean.
- Evidence: `python scripts/evidence/audit_evidence.py verify --baseline
  pi-v1.3.0` → **45 bundles, 101 artifacts, 562 scenarios**, including the new
  `audit-1099` family captured from the frozen checkout and registered by
  `check`/`register`. Consolidation `build`/`check` → **1,464 rows**, **559
  scenarios**, **141 explicit orphans** (the new one names the #1099 capture),
  zero uncovered.

Review findings addressed in the same change: the test cases heap-own the state
their remote callbacks write (`CODING_STANDARDS.md` §11.10); `start` option
validation and `deliver` admission are shared with the local sandbox instead of
duplicated; the serve loop batches `started` with the next call or idle, as the
frozen capture shows; `execute` failures keep the `Remote sandbox failed: ...`
taxonomy; and the poll and release-store sites carry their required comments.

## Capability status

No capability row is promoted. The `cch_codemode:codemode-remote` row keeps its
current status: this record supplies a deterministic-replay comparison, not the
whole-row receipt that row requires. #1179 owns whole-row codemode
reconciliation.

## Remaining boundary

The replay caller that detects an exhausted input journal is not part of this
slice; #1171 owns durable interrupted-journal continuation and #1119 owns
application store/resume. #1044 stays open. Magpie-only AI access, the XDG state
root, strict no-exception behavior and the ADR 0075 exclusions are unchanged.

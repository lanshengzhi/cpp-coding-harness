# Root-disk timing diagnostics for the two #1232 follow-up failures (2026-11-11)

## Scope

Diagnose the two test failures that appeared only after the #1232 test-harness
change made the Catch2 isolation root follow `TMPDIR` onto a btrfs root disk,
and fix what the diagnosis establishes. The two observed failures are
`deferred User Bash persistence failure ...` (a Run never resumed inside the
fixture's 30 s budget) and `repeated tool_search calls ...` (`runtime admission
capacity is saturated`). Both passed on tmpfs and failed on btrfs. No path
length is involved: the failures reproduce with roots under
`/var/tmp/cch-steps3/btrfs/cpp-harness-catch2-<pid>-<stamp>-0/…`, well below any
path bound, and they do not depend on the longer `/var/tmp` spelling (the same
binary, same env, only the filesystem differs).

Both cases live in `cch_tests_coding_agent`:

| Case | Source | CTest |
| --- | --- | --- |
| `deferred User Bash persistence failure is reported without rolling back Live Session State` | `tests/coding_agent/runtime/UserBashOverlapTest.cpp:544` | `ctest -R 'deferred User Bash persistence failure'` |
| `repeated tool_search calls in one run load each ranked match once and keep them declared` | `tests/coding_agent/McpSessionIntegrationTest.cpp:780` | `ctest -R 'repeated tool_search calls'` |

Reproduction used the isolated binary directly (same binary CTest invokes) with
`TMPDIR` set to a fresh directory on the filesystem under test; CTest
`-j 4` reproduces identically, so neither failure needs concurrency.

## (A) Deferred Bash persistence: the counted failure lands on the wrong append

**Phenomenon.** On btrfs the case failed 20/20 (30034–30086 ms each, the
`RuntimeFixture` `wait_budget`), on tmpfs 0/20 (43 ms max). The failure surfaced
as `RuntimeFixture.hpp:91: REQUIRE(completed)` — the fixture's 30 s budget, not a
product timeout: the scenario coroutine never reached its end. The earlier #1232
record read this as "the 293-byte append completes but the run does not resume".

**Mechanism (traced, temporary `fprintf` instrumentation, reverted).** The
session file is created by the deferred first flush (ADR 0064), and that flush
is itself a *counted* append attempt:

1. `JsonlSessionStore::flush_with_trigger_line` → `SessionJournal::flush_new`
   writes the header plus the buffered entries (including the prompt's user
   entry), `write_new_file_exclusive` returns, and only **then** does
   `flush_new` consume the injected-failure counter.
2. The test's `journal_contains(session_path, "failing persist run")` observes
   the file as soon as the write is visible — on btrfs during the `fsync`
   inside `write_new_file_exclusive`, before the flush's consume.
3. The test then arms `fail_nth_append_for_test(path, 2)` inside that window, so
   the flush consumes attempt 1 and the counter is left at 1.
4. The run's assistant append consumes the last attempt and fails: trace
   `flush_new lines=4` → `ARM attempt=2` → `consume remaining before=2` (flush
   thread) → assistant `append_line` → `consume remaining before=1` →
   `INJECTED FAILURE taken on append_line`. The deferred Bash append then runs
   unarmed and succeeds (journal ends with the `bashExecution` entry and no
   assistant entry).

The product behaved correctly in every step: the failed assistant append vetoed
the Run (`prompt_result` = `could not persist session entry`), the Bash commit
completed with the diagnostic and advanced Live Session State, and the sticky
persistence failure rejected the follow-up prompt. The test then waited for
`client_pointer->requests.size() == 2`, which can never happen once the follow-up
is rejected, so the real failure was converted into a 30 s budget timeout.

**Attribution: (ii) test assumption**, two independent parts.

- The arm point assumes the counted failure pins "the next append after the user
  entry", but the deferred flush consumes one counted attempt itself and the
  file becomes visible before that consume. The window is a few instructions on
  tmpfs (never hit in 20 runs) and milliseconds wide on btrfs (hit in 20/20),
  which is exactly the observed filesystem split.
- The follow-up wait is unbounded, so any rejected prompt becomes a 30 s hang
  instead of a discriminating assertion.

No lost wakeup or missing poll exists in the Bash deferral: `run_user_bash`
deferred while `prompt_active_`, `execute_run` flushed after the Run's messages
drained, and `commit_user_bash_completion` reported the store result. The 30 s
was the test's backstop, not a product timer.

**Fix.**

- A semantic test hook, `fail_next_bash_execution_append_for_test(path)`, fails
  exactly the next `bashExecution` line on the path, independent of preceding
  appends and of the deferred flush. The role match is the serialized pi wire
  value and stays inside the test-hook code; the production journal still never
  inspects a line. The counted hook is unchanged (the first-flush failure test
  still needs it).
- The case now waits for `requests.size() == 2 || second_prompt.has_value()`,
  releases the provider only when the request arrived, then waits for the
  prompt's settlement, so a rejected follow-up reports its typed failure.
- New guard: `the Bash-append failure hook skips earlier appends and the deferred
  first flush` pins the hook's semantics deterministically on any filesystem.

**Evidence.** Before the fix: btrfs 0/20 pass, tmpfs 20/20. After: 20/20 on both
(max 81 ms btrfs, 43 ms tmpfs).

## (B) `runtime admission capacity is saturated`: one Run's own chain exhausts the reserved lane

**Phenomenon.** The case failed on the first btrfs run in 98 ms (no waiting, no
timeout), with the Run's prompt returning `Busy`. tmpfs passes; `-j 4` is not
required.

**Mechanism (traced).** The error comes from
`SessionPersistence::submit_append` when `RuntimeTarget::try_admit_reserved`
returns nullopt. Every submit charges one reserved operation plus
`estimate_message_bytes + 4096`; the charge is released only when the append's
terminal reaches the target mailbox (`Admission::complete` → mailbox drain).
A Run's `MessageEnd` sink submits without waiting for the previous append and
the channel drains at `conclude()`, so one Run's dependent chain holds one
operation per pending append. Traced counts on btrfs, one Run of this test:

```
reserved admit ops=1..8, bytes=4096..32768, charge=4096  (12 appends submitted)
RESERVED REJECT ops=8 max=8 bytes=32768 charge=4096
reserved release ops=7,6,5,4,3,2,1,0      (all at once, after the chain drained)
```

Admissions are all held by this Run's own appends (charge 4096 each, byte use
32 KiB of the 256 KiB budget); the fixture creates a fresh `RuntimeRoot` per
case, so no earlier case can hold them. The release burst at the end proves
nothing leaked: the lane simply cannot absorb the chain.

**Attribution: (i) product defect — reserved-lane sizing/accounting.** The
reserved lane is documented as covering "one in-flight append plus queued
appends ... several concurrent sessions' persistence"
(`docs/runtime-capacities.md`, selection rule 4), but it is charged for every
*pending* append, and the selected count of 8 is below one ordinary Run's chain:
this test emits 12 appends for four model turns with three `tool_search` loads.
Measured peak outstanding with the bound lifted: 5–6 on tmpfs, 10–11 on btrfs.
The rejection also latches `first_failure`, so a transient capacity miss
permanently disables the session — every later prompt is refused. The byte
budget was 12.5 % used, so the operation count, not memory, was binding.

**Fix (TDD).** Red first: `one run's dependent append burst is admitted on the
reserved control lane` submits twelve dependent appends at the production
default limits, drains, and requires all twelve persisted in order. At 8 it
failed on the eighth append on **both** filesystems (deterministic, no slow
machine needed); at the new value it passes on both. `max_reserved_operations`
is now 64, which multiplied by the 4096-byte admitted-operation overhead equals
the 256 KiB byte ceiling: the byte charge becomes the binding memory contract
and the task count only bounds per-append bookkeeping.
`docs/runtime-capacities.md` selection rule 4 records the measurement, the
re-selection, and the regression property.

**Residual boundary.** A chain deeper than 64 pending appends (about twenty
zero-latency tool rounds in one prompt) still returns `Busy` and still latches.
The structural upgrade is ADR 0040's reserved waiter set — required persistence
work waiting asynchronously for capacity instead of being rejected. It is not
implemented here; the measured shape is one serialized chain per Session and
the bound is documented rather than silently retuned.

## Excluded hypotheses

- **Path length / relocation.** Both failures reproduce under a short root on
  `/var/tmp` and are absent under `/tmp`; (A)'s trace shows the arm landing
  between the flush's write and its consume, (B)'s counts show an 8/8 admission
  rejection. Neither involves a path.
- **(A) as a lost wakeup.** No timer, poll, or wakeup is missing: the deferred
  Bash completed, committed, and reported its diagnostic; the missed event was
  an *expected* prompt request that the product had correctly refused.
- **(B) as a leak or delayed release from earlier cases.** Each case builds its
  own `RuntimeFixture`/`RuntimeRoot`; the traced reservations belong to this
  case's own append chain and are released in one burst when it drains.
- **Concurrency-only.** Both cases fail (B) or hang (A) when run alone;
  `-j 4` reproduces the same outcomes.

## Validation

Build: `cmake --build --preset vcpkg` (the production default in
`RuntimeRoot.hpp` changed). Repeated focused runs used a fresh `TMPDIR` per
batch; the btrfs batches used `/var/tmp/cch-steps3/btrfs`.

| Case | tmpfs (20×) | btrfs (20×) |
| --- | --- | --- |
| deferred User Bash persistence failure (before) | 20 pass | 0 pass, 30034–30086 ms each |
| deferred User Bash persistence failure (after) | 20 pass, ≤43 ms | 20 pass, ≤81 ms |
| repeated tool_search (after) | 20 pass, ≤138 ms | 20 pass, ≤219 ms |
| one run's dependent append burst (red at 8) | fails at append 8 | fails at append 8 |
| one run's dependent append burst (after) | 20 pass | 20 pass |
| the Bash-append failure hook | 20 pass | 20 pass |

`ctest --preset vcpkg -j4 -R '<the four cases>'` with `TMPDIR` on btrfs: 4/4.

- Owning label on btrfs (`TMPDIR=/var/tmp/cb`, `TERM_PROGRAM=herdr`,
  `ctest --preset vcpkg -LE architecture -L coding_agent -j4`): **selected 1889,
  passed 1888, skipped 1, failed 0, 65.67 s**.
- Full Debug suite on tmpfs (`TERM_PROGRAM=herdr`, `ctest --preset vcpkg -j4`):
  **selected 4595, passed 4584, skipped 11, failed 0, 430.11 s** (4593 before
  the two added cases; the 11 skips are the pre-existing pi/SQLite
  interop cases).
- `consolidate_audits.py check`: PASS; none of the changed files is a pinned
  delivery input, so no receipt re-validation was needed.
- `scripts/format-check.sh`: clean.

**Adjacent finding, not fixed here.** A `coding_agent` label run with
`TMPDIR=/var/tmp/cch-steps3/btrfs` failed six TUI cases (`/import` twice, the
footer usage totals, the late clipboard payload, the colliding import, and the
missing stored cwd). All six assert `screen.find(<workspace path or text>)` or
the `pi-clipboard-` label, and all six pass with the same btrfs filesystem under
the shorter `TMPDIR=/var/tmp/cb` and with `/tmp`. They are pre-existing
path-length assumptions in TUI screen assertions (the row wraps or truncates
once the isolation root is long enough), unrelated to the two failures above
and to this change; the btrfs label run above therefore uses a short btrfs
root. They should get the same unwrapped-text treatment the #1232 TUI fixes
gave the other cases.

## Capability status

No capability row advances. (A) is a test-harness correction; (B) restores the
documented reserved-lane contract for one Run's persistence chain and changes no
Runtime, Session, or provider observable behavior other than the previously
session-fatal capacity cliff.

## Remaining boundary

- The reserved lane is still a hard rejection for a chain deeper than 64 pending
  appends; ADR 0040's reserved waiter set is the structural fix and remains
  unimplemented.
- `commit_user_bash_completion` appends through `SessionStore::append` directly
  on the Runtime loop (a synchronous fsync on the loop thread) rather than
  through the persistence channel. Observed while tracing (A); it did not cause
  either failure because `execute_run` flushes only after the Run's appends
  drained. It remains a blocking-work-on-the-loop deviation from ADR 0040 and is
  not changed here.
- CI still does not export `TMPDIR` (recorded in
  `1232-test-tmp-root-install-diagnostics-2026-11-11.md`), so neither the btrfs
  reproduction nor these fixes run there.

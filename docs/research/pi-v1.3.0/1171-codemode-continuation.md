# #1171 — bidirectional interrupted codemode continuation

## Scope

Delivers the frozen durable codemode task/document/journal identity and
reattachment needed for frozen pi and this runtime to continue one interrupted
script in either direction, under
[#1180](https://github.com/lanshengzhi/cpp-coding-harness/issues/1180),
[#1183](https://github.com/lanshengzhi/cpp-coding-harness/issues/1183) and
[#1044](https://github.com/lanshengzhi/cpp-coding-harness/issues/1044), under
[ADR 0075](../../adr/0075-align-selected-owner-packages-and-linux-capabilities-with-pi-v1-3-0.md).
Named baseline: `pi-v1.3.0` / `71023aec1546d6d677a65669c5665c0927ce0dd0`,
Linux x86-64 with glibc.

#1094 stored a bounded run document under a generation/model-call key in this
runtime's own shape. #1170 replaced serial nested calls with independent
`pi.tool` tasks but kept that run shape and explicitly left journal/task
interchange and interrupted cross-runtime continuation here. This slice adopts
the frozen documents and task identities rather than defining a second format.
Whole-row verification, capability promotion and the ledger rebuild stay with
#1180.

## Frozen contract

Source of truth at the pinned revision:

- `packages/durable/src/codemode/codemode.ts` — `CodemodeRunDoc` (`pi.codemode.run`,
  version 1), `CodemodeJournalDoc` (`pi.codemode.journal`, version 1),
  `CodemodeStoreDoc`, `CodemodeCall.launch`/`pump`/`deliverReplayed`/`onIdle`,
  the fingerprint gate, the divergence rules and `outcomeOf`.
- `packages/durable/src/harness/tool.ts` — `ToolTask` (`pi.tool`, version 2),
  both inputs (`kind: "model"` and `kind: "nested"`), `NestedCallDoc`
  (`pi.tool.nested-call`, version 1), `settle`, `finalResult`, `executeTool`.
- `packages/durable/src/storage/` — the SQLite and JSONL entry records, whose
  application payload is flattened beside `model`/`byTaskId`.

Document shape now written and read by this runtime:

- `pi.codemode.run` is one task-scoped singleton holding `start` (module version
  2, clock, 32-bit seed, fingerprint, optional deadline, tools, namespaces,
  globals, output-token budget and store snapshot) and the one `result`.
- `pi.codemode.journal` is one task-scoped singleton holding `entries` in
  delivery order (`id`, `at`, optional `issuedAfter`, optional global outcome)
  and `firstIssued`. A tool's answer is read from its nested call's result, not
  copied into the journal; a global's outcome is journaled.
- `pi.tool.nested-call` members carry the frozen `taskId` and result
  (`structuredOutput`, `isError`, `details`, diagnostics, duration). The
  pre-#1171 `content` member is no longer written; an older result that has no
  `structuredOutput` is decoded from its content for local continuity.
- A model-issued `pi.tool` task resolves its call from the committed assistant
  entry and settles a `pi.tool-result` entry with the frozen
  `data.diagnostics` payload. Both backends accept frozen pi's flattened entry
  records and ignore retired documents whose revision history pi removed.

## Delivered behavior

`src/durable/Codemode.cpp` now persists the frozen run and journal, writes the
store under the frozen `pi.codemode.store` kind, records `firstIssued` and
`issuedAfter`, delivers journal entries by result number, reattaches replayed
tool calls by key, preserves recorded clock/seed/fingerprint and refuses record
counts or call identities that changed. Fingerprint strings produced by the C++
VM and frozen pi are identical for the default run options, so each runtime
accepts the other's recorded start.

`src/durable/ToolTask.cpp` now implements both frozen `pi.tool` inputs. The
model path reads `{assistant, callId}` from the committed entry, runs the same
hooks and replay policy as a nested call, and settles a `pi.tool-result` entry.
The nested path writes the frozen nested result. Cancellation of either path
settles the result its caller waits on.

`src/durable/JsonlStorage.cpp` and `src/durable/SqliteStorage.cpp` reconstruct
the application payload from frozen pi entry records and skip already-retired
documents on recovery, while retaining this runtime's legacy payload-under-`data`
reading. Nothing migrates a historical Pike store or falls back to the #1094 run
shape.

Frozen observations are taken from the exact revision rather than derived from
C++: the registered `audit-1094/codemode.json` replay-policy scenarios are reused,
and the missing runtime-interchange observations are captured by executing the
frozen public Harness in both directions through
`docs/research/pi-v1.3.0/continue-1171-codemode.mts`. A snapshot-only comparison
would fail the case, because each leg must append journal entries, settle the
parent task and preserve the store before the other runtime reads it.

The live comparison drives the public Harness on both runtimes over one JSONL or
SQLite store. A registered parent task starts a model-issued `pi.tool` codemode
task. The script commits one `effect` answer, pauses, is SIGKILLed, and is
continued by the other runtime through a second pause, then finished back in the
first runtime. Both directions preserve one effect call, the original
clock/random stamp, the committed journal entries and the final store, and both
backends are exercised. The divergence case rewrites the committed
`issuedAfter` frontier and requires an explicit "different path" error without
repeating the completed effect.

## Recorded divergence and remaining boundary

Ordinary model generation in this runtime still executes a model-issued call
inline in the `pi.generation` task, as recorded by #1170. The cross-runtime tests
therefore create the frozen model `pi.tool` task directly through a registered
parent task; making every generation-issued call its own `pi.tool` task remains
the generation-scheduling divergence. The codemode task/document/journal graph
itself uses frozen identity and ownership, and a pi-created model tool task is
continued on the same terms.

This slice does not claim exactly-once physical effects: a replay-safe operation
interrupted after its effect but before its answer commits may repeat that
effect. It does not add output budgets, spill files or image files (#1172),
Remote exchange replay (#1099), the ordinary application codemode store (#1119),
power-loss or multi-process guarantees, or whole-row verification (#1180).
#1044 remains open.

## Tests

All live cases live in `tests/durable/CodemodeContinuationTest.cpp` and spawn
frozen pi at the named revision through
`docs/research/pi-v1.3.0/continue-1171-codemode.mts` or a hidden C++ child of
the same test binary, so each leg is a real process interrupted at a committed
boundary.

- `pi and C++ continue each other's interrupted codemode run` — pi creates,
  C++ continues to the second pause, pi finishes, and C++ reopens the terminal
  run. Runs on SQLite and JSONL; checks the frozen version/frontier shape after
  each interruption.
- `C++ creates an interrupted codemode run pi continues` — the opposite
  direction, including a final pi reopen after C++ finishes.
- `a divergent codemode frontier fails explicitly` — a changed `issuedAfter`
  count is rejected without repeating the committed effect.
- `a serial-era codemode run is refused` — a #1094 family run document under the
  task is reported as a tool error instead of being silently treated as a fresh
  frozen run.

## Validation

- Focused Validation: default strict Debug build; `ctest --preset vcpkg -L
  '^issue1171'` selected **4** cases and passed **4/4**. JUnit receipt:
  `deliveries/1171-focused.xml` (live cases ran with the frozen checkout).
- Affected regression lanes: `issue1094` and `issue1170` selected **21** cases
  and passed **21/21** with the frozen checkout (the #1170 entered-order case is
  wall-clock-sensitive under a loaded host and passed on the repeated run);
  JUnit receipt: `deliveries/1171-revalidation-existing.xml`.
- The changed `cmake/tests/DurableTests.cmake` input pinned by the #1090
  delivery was reviewed: no #1090 scenario mapping or selected case changed.
  The exact `^(issue1090|issue1101|issue1104)$` selection reran **36** selected,
  **36** passed; `deliveries/1105-existing-deliveries-revalidation.xml` and the
  #1090 input digest in `1058-deliveries.json` were refreshed. Consolidation
  `build` and `check` passed (1,464 rows, 568 scenarios, 362 named by a row,
  56 artifact-covered, 150 explicit orphans, 0 uncovered).
- Full Validation: `ctest --preset vcpkg -j1` selected **4,151** cases,
  passed **4,149**, skipped **2** and failed **2**. The failures are known
  runner-sensitive cases outside this slice: the quarantined #632 100-chunk
  projection timing bound (172/173 assertions passed; 2.098 ms against the 2 ms
  bound) and the #1019 Process Terminal postdrain-EIO case. The terminal case
  passed **5/5** isolated reruns; the quarantined projection case passed
  isolated runs but remains load-sensitive. Both skipped cases are the
  documented realtime-clock and Native TUI differential cases.
- Consolidation: `consolidate_audits.py build` and `check` passed (1,464 rows,
  568 scenarios, 362 named by a row, 56 artifact-covered, 150 explicit orphans,
  0 uncovered); `ConsolidationAuditTest.py` passed **42/42**;
  `audit_evidence.py verify` verified **47** bundles, **103** artifacts and
  **571** scenarios at the named revision.
- `scripts/format-check.sh 6764d6bcf` and `git diff --check` passed.

## Capability status

No capability status or membership changes. The interrupted continuation
property is verified for the selected task graph on both backends; whole-row
promotion and its remaining output/replay dependencies stay with #1180.

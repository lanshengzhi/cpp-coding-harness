# #1094 — durable codemode store, nested tools and restart replay

## Scope

User-approved batch B36 / F23c, under #1044 and ADR 0075. Named Baseline:
`pi-v1.3.0`, revision `71023aec1546d6d677a65669c5665c0927ce0dd0`, Linux x86-64/glibc.
This slice integrates the independent `cch_codemode` Owner directly into durable Harness.

## Delivered behavior

- `codemode_extension` owns a reusable sandbox and registers a context-aware durable tool.
  Model-written scripts invoke the request's registered tools through the ordinary before/after
  tool hooks. Real write, edit, read and bash operations run over an injected local environment.
- `pi.codemode.store:v1` is a rewindable conversation document with as-of fork semantics.
  Successful writes and the script outcome commit together. Failed or aborted VM outcomes leave
  the previous store intact; deletion and branch isolation are exercised through Harness.
- A task-scoped, keyed run document records source, module version, tool surface, starting clock,
  seed, store snapshot, VM fingerprint, issued-call acknowledgements and delivered answers.
  Identities and issued frontiers commit before nested execution; each answer commits before VM
  delivery. Restart reuses these inputs and answers instead of repeating committed nested effects.
  A durable admission marker separates unfinished operations from calls merely issued by the VM;
  interrupted operations rerun only with explicit `replay_safe` opt-in. Ordinary unfinished tools
  without that opt-in retain their prior recovery behavior.
- Generation recovery examines committed context contributions before model-context repair.
  It completes replay-safe missing tool results before requesting another assistant response. Restoring
  assistant tool-call blocks and tool-result details keeps the resumed request faithful to history.
- Script text, return values, console markers, image blocks, errors and nested-call rows return
  through the committed Harness conversation view. The abort transaction preserves the unfinished
  script's previously committed nested history before task-document retirement. Completed nested
  outcomes commit even when the abort mark wins before their result is delivered; a successful VM
  outcome and its store writes use the same non-cancellable Session commitment.

The two initial probes separated real gaps: the first public submission returned "tool unavailable";
then a restart lost the missing tool result because model-context repair synthesized one. The
smallest successful two-turn script/store probe took 2.35 seconds; the VM startup dominates this
small case. Run state is currently rewritten per answer, with its quadratic long-history cost
recorded in the implementation.

Independent comparisons reuse the registered `audit-1056/codemode-toolsearch.json` script/store
and failure scenarios. The new registered `audit-1094/codemode.json` observes mixed text/image/
console/value output and SIGKILL restart with both safe and unsafe waiting tools against frozen pi.
Its capture validates the revision and clean checkout and hashes its generator and helper sources.
The C++ SIGKILL comparison runs safe and unsafe cases on both SQLite and JSONL: a safe interrupted
call resumes with the original clock/random stamp in output and store; an unsafe interrupted call
returns an error without repeating its operation. Both preserve the first completed nested answer.

The frozen graceful-close probe distinguished a nested tool's replay policy from a completed
answer: an interrupted tool without `replay: "safe"` yields an error on restart. The registered
crash observations exercise both declarations. The C++ graceful-reopen regression separately
exercises its current inline-tool continuation behavior.

## Validation

- Focused Validation: default strict Debug build of `cch_tests_durable`; CTest label `issue1094`
  selects 14 cases, all passing. The SIGKILL case runs both persistent backends and both replay
  policies. Every selected case has a 30-second CTest process timeout.
- Comparisons cover successful script/store behavior, exact frozen failure text and call metadata,
  mixed image output, real file/shell results, nested errors and tool hooks, failed-write rollback,
  cancellation history, as-of store forks and deletion, graceful reopening, and SIGKILL replay.
- The affected #1090 image selection was rebuilt and revalidated: 10 selected, 10 passed. Its
  receipt refreshes only the reviewed changed shared build/document/registry inputs and retains
  the original image scenario routing and earlier JUnit artifact.
- Named-evidence verification and both consolidation checks pass with the registered frozen
  checkout. The new capture has three observations; no capability status was promoted.
- Independent Standards and Spec reviews have no unresolved findings after replay-safety,
  completed-result persistence, checked serialization and shared-wait-helper fixes.
- Full Validation: `cmake --build --preset vcpkg` and the complete unfiltered
  `ctest --preset vcpkg` selection passed on the final code: 3,970 selected, 3,969 passed,
  zero failures, one optional Native TUI differential-capture case skipped. The local
  `PI_CHECKOUT=/tmp/cch-1080-pi-frozen` enabled the frozen local capture/interchange checks;
  no provider credentials or network validation was used.
- Architecture selection: 43 selected, 43 passed, including production ownership/dependency
  evidence, standalone Owner Interface compilation, compiler warnings and strict no-exception
  checks. The direct `cch_durable` → `cch_codemode` edge is in the manifest and build graph.
- `scripts/format-check.sh 64c0f19d14cc772cf7e99bef8b2d2311ed8704c6` passed; `git diff --check`
  passed. Focused JUnit receipts are committed under `deliveries/1094-focused.xml` and
  `deliveries/1090-revalidation-1094.xml`.
- The earlier Full attempt exposed the expected stale shared delivery pins and a sibling pi
  checkout missing esbuild's platform binary. Revalidation refreshed only the affected #1090
  pins, and the final Full run used the clean, working frozen checkout. No gate was relaxed.

## Capability status

Membership and existing whole-row statuses are unchanged. The script/store and failure comparisons
now have passing C++ evidence, but this bounded delivery does not add a whole-row promotion receipt.
The new observations are registered as explicit consolidation orphans associated with their rows.
Script execution, replay and output remain partial: the successful comparisons do not verify
independent nested task identity, parallel execution, interrupted pi/C++ journal continuation,
output spilling or environment-backed image saving. Limits, model globals, tool search, Remote
replay and the application codemode store are not promoted by this slice.

## Remaining boundary

Nested operations currently execute serially on the generation thread, and their persisted run
state is keyed by model call ID under that generation task. It is not the complete frozen
`pi.tool` / `CodemodeRunDoc` / `CodemodeJournalDoc` task graph. In particular, this evidence does not
establish bidirectional pi/C++ continuation of an interrupted codemode call. That target remains
an implementation gap; no legacy format reader, migration or fallback was added.

Output returns the frozen explicit no-execution-environment image label and unchanged image bytes.
The broad runaway text bound is present; token-budget spill files, image files, full output-limit
parity and independently cancellable parallel nested tasks remain implementation gaps. A replay-safe
operation interrupted after its physical effect but before its answer commits may repeat that effect;
this is not an exactly-once physical-effect guarantee.

SIGKILL observations do not establish power-loss, fsync-failure or concurrent-writer guarantees.
The ordinary Pike CLI Session, XDG root and Magpie-only model boundary remain separate. No live
model provider, third-party pi TS/JS plugin or terminal-emulator claim is involved. #1044 remains open.

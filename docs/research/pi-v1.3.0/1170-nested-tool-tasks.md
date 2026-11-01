# #1170 — independent durable nested ToolTasks and cancellation

## Scope

Delivers independent durable nested `pi.tool` tasks, their ownership, replay
policy and cancellation under
[#1180](https://github.com/lanshengzhi/cpp-coding-harness/issues/1180),
[#1183](https://github.com/lanshengzhi/cpp-coding-harness/issues/1183) and
[#1044](https://github.com/lanshengzhi/cpp-coding-harness/issues/1044), under
[ADR 0075](../../adr/0075-align-selected-owner-packages-and-linux-capabilities-with-pi-v1-3-0.md).
Named baseline: `pi-v1.3.0` / `71023aec1546d6d677a65669c5665c0927ce0dd0`,
Linux x86-64 with glibc.

#1094 delivered codemode over the `cch_codemode` Owner with serial nested
operations on the generation thread. This slice replaces that execution shape
inside `cch_durable`: every nested call a script makes becomes its own durable
task, owned by the calling task, keyed in a task-scoped document family. The
calls run concurrently, commit their own result before the VM sees it, survive a
restart under the recorded replay policy, and report cancellation into the
member their caller waits on. Whole-row verification, capability promotion and
the ledger rebuild stay with #1180. Interrupted cross-runtime journal
continuation stays with #1171.

## Frozen contract

Source of truth, read at the pinned revision:

- `packages/durable/src/harness/tool.ts` — `ToolTask`, `NestedCallDoc`,
  `ToolTaskInput` (`kind: "nested"`), `startNestedCall`, `executeTool`,
  `settle`, `finalResult`, and the `abort` handler.
- `packages/durable/src/codemode/codemode.ts` — `CodemodeCall`: `launch`,
  `pump`, `deliverReplayed`, `onIdle`, the run/journal documents and the
  `callTool` → `api.executeTool(..., { key: 'c<id>' })` binding.
- `packages/durable/src/harness/scheduler.ts` — `#gated` commit rules, abort
  cascade and the `waiting`/`completing` lifecycle.
- Registered observation `fixtures/pi-v1.3.0/audit-1170/codemode-nested.json`,
  captured by `fixtures/pi-v1.3.0/audit-1170/capture-1170.mts` from the exact
  frozen revision and registered in `fixtures/pi-v1.3.0/baselines.json`.

Six frozen scenarios are registered:

1. `independent nested calls run concurrently and deliver in completion order`
2. `cancelling one nested task leaves its sibling and the script running`
3. `cancelling the parent codemode call aborts its nested work`
4. `cancelling an observer wait does not cancel the nested call`
5. `nested calls use the public tool hooks with their parent`
6. `a script that ends without awaiting its nested call admits no call`

Frozen behavior implemented here:

1. **Independent calls.** A script's nested call is admitted only after the
   issued frontier commits, then runs as its own `pi.tool` task. Two calls the
   script starts together are live at the same time; releasing them in reverse
   order makes them finish, and be delivered, in that reverse order. The
   `details.calls` rows stay in issue order, while the answers reach the VM in
   journal (completion) order.
2. **Durable identity and ownership.** Each nested call's task records
   `input.kind: "nested"` with its `parent`, `parentCallId`, `key`, and the
   call's id/name/arguments; its `owner` is the calling task. The caller's
   `pi.tool.nested-call` member records the created task ID before the intent
   commits, so a rerun reattaches to the same call by key instead of repeating
   it. A key that names another tool or other arguments is rejected.
3. **Replay policy.** The `call` phase commits the execution intent
   (`{phase: "execute", arguments, replay}`) before the physical operation. A
   recovery from `call` may run the operation (it never started); a recovery
   from `execute` reruns only when both the recorded and the current policy are
   safe, else the call reports `Tool <name> was interrupted and may have
   partially run`.
4. **Three cancellation effects.** Cancelling one nested task settles that call
   `aborted` (the script's rejection is `Tool <name> was aborted`, its row is
   `error` without a duration) while its sibling and the script continue;
   cancelling the parent aborts the nested work, discards the script's store
   writes and reports one harness error; cancelling an observer wait rejects
   only that wait.
5. **Public tool path.** Nested calls run the registered `pi.tool`
   `beforeTool`/`afterTool` hooks with `parent: {taskId, callId}`, and an
   `afterTool` replacement is what the script receives.
6. **No eager admission.** A call the script starts and never awaits is never
   admitted: its row stays `cancelled` and no nested task exists.

## Delivered behavior

- `src/durable/include/cch/durable/Task.hpp`: `TaskRuntime` gains the frozen
  nested-task seam — `callable_tools()`, `task_definition(kind)`,
  `wait_for_task(id, stop)`, `abort_task(id)` and `commit_result(change)`. A
  phase creates a child inside one of its own commits with
  `Transaction::create_task`, so the child and the caller's own records commit
  together. `commit_result` is the abort-tolerant settlement a nested call
  needs: a tool that already returned keeps its observed outcome instead of
  losing it to a concurrent abort mark.
- `src/durable/ToolTask.{hpp,cpp}` (new): the built-in `pi.tool` version 2
  definition (`call`/`execute`/`abort`), `start_nested_call`, the task-scoped
  `pi.tool.nested-call:v1` family document, the stored result codec and the
  `executeTool` recursion a nested call's own nested calls use.
- `src/durable/ToolContent.{hpp,cpp}` (new): the content-block codec and the
  harness error content, extracted from `WorkflowRuntime.cpp` so the nested
  task and the generation path share one encoding.
- `src/durable/JsonFields.hpp` (new, private): the JSON member/integer/number
  and validation helpers the generation path already carried, now shared with
  the nested task and content codec instead of copied a third time.
- `src/durable/Codemode.cpp`: `Execution` launches every admitted call as a
  nested task and waits for it beside the event loop, records its row and
  journal entry before delivering the answer, delivers recorded entries in
  journal order, and aborts calls it leaves behind only when the script decided
  its outcome. The run state moves to shape version 3: a document from the
  serial #1094 shape is refused rather than replayed by different semantics.
  The parent-abort result is the frozen harness error
  `<harness>[error] Tool codemode was aborted</harness>` with no details; the
  #1094 script-history rendering is superseded by the nested task records.
- `src/durable/Harness.cpp`: `HarnessTaskRuntime` implements the new seam from
  the invocation's registry snapshot, `Impl` owns the built-in `pi.tool`
  definition and the extracted `wait_for_task`/`abort_task` operations, and the
  conversation-abort pass reads every reached task from its own scan instead of
  re-reading each record after the commit's first table write.
- `tests/durable/NestedToolTaskTest.cpp` (new): the six registered frozen
  comparisons plus a direct recovery case for the recorded replay policy.
- `tests/durable/HarnessCodemodeTest.cpp`: the two #1094 abort cases now assert
  the frozen harness error and the durable nested task records instead of the
  superseded script-history text.

Recorded divergence, not fixed here: frozen pi makes a model-issued call its
own `pi.tool` task, so a script's nested calls are owned by that task. This
runtime still executes a model-issued call inline inside the generation
invocation, so the generation task owns them. Task identity, ownership and the
abort cascade are preserved; the owner's kind differs, and the frozen
observation is compared on the nested tasks' own identity and ownership rather
than the parent's kind. Moving model-issued calls onto tasks belongs to the
generation-scheduling work, not to this ticket.

## Acceptance discipline

Each criterion carries a case the check would pass while the property is
violated:

- **Concurrency (criterion 1).** A serial generation-owned implementation never
  enters the second call, so
  `independent nested calls run concurrently and deliver in completion order`
  fails its `bothLiveBeforeRelease` comparison (and its bounded entry wait)
  while a script that awaits one call at a time still passes.
- **Reverse release order (criterion 1).** An implementation that delivers
  answers in issue order instead of journal order produces
  `completionOrder: ["A", "B"]` and a different hook log, which the frozen
  comparison rejects.
- **Identity and ownership (criterion 2).** A shallow implementation that keys
  only the journal row produces no `pi.tool` task records; the frozen
  `liveTasks`/`nestedTaskRecords` projections require `inputKind: "nested"`,
  `key`, `callId`, `ownerIsParent` and `resultKind: "nested"`.
- **Cancellation distinction (criterion 2).** The three cancellation cases are
  mutually separating: cancelling one task leaves the sibling's `ok` row and the
  script's success, cancelling the parent discards the store and reports the
  harness error, and cancelling a wait leaves the call's `completed` record.
- **Public path (criterion 3).** The hook case requires the `pi.tool` hooks to
  fire with `parent.callId` and the `afterTool` replacement to reach the script;
  a generation-owned serial implementation shows no nested hook entries.
- **No eager admission (criterion 3).** An implementation that launches on
  `on_call` instead of after the committed issued frontier runs the un-awaited
  call; the frozen case requires `entered: []` and `nestedTasks: 0`.

## Tests

All new cases live in `tests/durable/NestedToolTaskTest.cpp` and drive the
public Harness: a scripted model issues `codemode`, the registry supplies two
independently gated tools, and the tests observe the public result entry, the
public inspection seam, the public task reads, the nested-call documents and the
public cancellation calls.

- `Independent nested calls run concurrently and deliver in completion order`
  (`[issue1170]`): the registered discriminator; compares the whole frozen
  projection, including the live task shape, both nested-call members and the
  terminal task records.
- `Cancelling one nested task leaves its sibling and the script running`.
- `Cancelling the parent codemode call aborts its nested work`.
- `Cancelling an observer wait does not cancel the nested call`.
- `Nested calls use the public tool hooks with their parent`.
- `A script that ends without awaiting its nested call admits no call`.
- `A recovered nested call applies its recorded replay policy` (generator over
  `replay_safe`): a gated call is interrupted by a graceful close, the Harness
  reopens, and the resumed `execute` phase either reruns the call (safe, one
  completion, the script's value) or reports `Tool gate was interrupted and may
  have partially run` (unsafe, no completion).

Every case releases its gates, settles, closes the Harness and records its
observations before the discriminating assertions, so a failing assertion leaves
no live task thread (`CODING_STANDARDS.md` §11.10).

## Validation

- Focused: default strict Debug build of `cch_tests_durable`; CTest label
  `issue1170` selected **7** cases and passed **7/7**; the JUnit receipt is
  `deliveries/1170-focused.xml`. The owning module label
  `ctest --preset vcpkg -L durable` selected **310** cases: **309** passed, 6
  skipped optional frozen-checkout interchange cases, and one pre-existing
  `[issue1163]` workflow case whose 10-second in-test backstop timed out under
  host contention; it passes on isolated rerun (0.01 s) and is unrelated to this
  slice. The #1094 label re-ran on the new code: **14** selected, **14/14**
  passed.
- Architecture selection, required because `TaskRuntime` and the built
  `cch_durable` sources changed: `ctest --preset vcpkg -L architecture`
  selected **43** cases and passed **43/43**; the configure and build phases of
  the Parity Architecture Gate report PASS.
- Full Validation: `ctest --preset vcpkg -j 2` with `TERM_PROGRAM=herdr` (the
  prerequisite the PTY evidence case declares) on the final tree selected
  **4,111** cases: **4,104 passed, 7 failed, 8 skipped** while sibling lanes held
  the host at a load average above 7 on 8 cores. Every failure is a
  wall-clock-sensitive case outside this slice's changed behavior and passes on
  isolated rerun: the `[issue1163]` attempt-publication backstop, the alternate
  screen post-drain probe, bounded parallel cancellation, the codemode
  interrupt budget, the quarantined projection cost bound, the quarantined
  streaming-block benchmark and an `/mcp` reconnect probe. The 8 skipped cases
  are the existing optional live-pi/SQLite interchange, realtime-clock and
  Native TUI differential skips.
- Two earlier full runs on the same code are recorded for honesty. At `-j 2`
  with the same PTY prerequisite: **4,110 passed, 1 failed, 8 skipped**, the
  failure being a durable environment watch/timeout case that passes on isolated
  rerun (0.67 s). At `-j 4` (default preset): **4,106 passed, 5 failed, 8
  skipped**, all five passing on isolated rerun. Those runs also reported one
  `[issue1170]` hook-order failure, which led to a test-robustness fix: the case
  now waits for the released sibling's durable task record instead of a log
  entry, with a 30-second bounded backstop.
- `scripts/format-check.sh bbd499a4da471ddd0b956f0d74a03c2201d6b8fb` reports
  clean after `git clang-format` on the changed files; `git diff --check`
  passes.
- Named evidence: `scripts/evidence/audit_evidence.py verify` reports 47
  bundles, 103 artifacts and 571 scenarios verified for the named baseline. The
  six audit-1170 scenarios are registered as explicit consolidation orphans;
  `consolidate_audits.py build` and `check` pass (1,464 rows, zero uncovered
  scenarios), and `ctest --preset vcpkg -R cch_pi_v130_consolidation` selected
  **2** and passed **2/2**.
- Delivery pins: the four existing records whose pinned inputs moved with this
  slice (#1090, #1101, #1104, #1118) are revalidated on commit
  `8658bce9ba6eb577dec6900df9279b21254ec9c7`; the shared
  `^(issue1090|issue1101|issue1104)$` selection selected **36** and passed
  **36/36**, and `issue1118` selected **19** and passed **19/19**. Receipts:
  `deliveries/1170-revalidation-existing.xml`,
  `deliveries/1170-revalidation-1118.xml`.

## Capability status

`cch_durable:codemode-script`, `cch_durable:codemode-replay` and
`cch_durable:codemode-failure` advance implementation only. Their whole-row
`verification` statuses are unchanged, and the whole-row comparison against the
registered artifact remains owned by #1180. No capability row is promoted by
this slice.

## Remaining boundary

- Interrupted cross-runtime journal/task continuation in both directions, on
  JSONL and SQLite, remains #1171. This slice persists and replays within one
  runtime; it does not interchange a codemode journal with frozen pi.
- A model-issued call is still inline in the generation invocation, so the
  generation task owns a script's nested calls (recorded divergence above).
- A nested call's own `ToolExecutionContext::execute_tool` seam stays unwired.
  The model-issued call excludes its own tool from a script's tool list, so a
  script cannot reach a nested codemode call and admit grandchildren; wiring
  that seam without that change would be unreachable code.
- Output spilling, image files and full output-limit parity remain #1172.
- A nested call whose tool ignores cancellation can hold its caller as
  `completing`; no timeout or forced termination is introduced here.
- SIGKILL observations do not establish power-loss, fsync-failure or
  concurrent-writer guarantees. The ordinary Pike CLI Session, XDG state root
  and Magpie-only model boundary are untouched, and no live model provider is
  involved. #1044 remains open.

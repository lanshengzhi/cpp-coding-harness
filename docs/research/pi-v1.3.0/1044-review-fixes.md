# Review fixes for the implemented #1044 slices

## Scope

Six findings from the review of `98d7ab562...0f52fcb3a`, limited to the implemented durable environment, wait and compaction contracts and their callback/synchronization rules. The user authorized direct replacement, with no compatibility adapters, fallback paths or migrations. This does not complete the remaining #1044 capability inventory.

## Delivered behavior

- Environment execution keeps cancellation and the deadline active after the command process exits. Inherited stdout/stderr are drained until 100 ms without output, matching frozen pi's `EXIT_STDIO_GRACE_MS`; every chunk resets that grace. Reading one chunk per ready stream also returns control to cancellation/deadline processing under sustained output. An established timeout remains final if cancellation arrives later during the drain.
- A closed or failed `Harness::wait_for_idle()` copies its error while locked, then invokes its completion after releasing the scheduler mutex. That completion can query `wait_for_task()` without a recursive lock acquisition.
- Compaction tool text uses the frozen 2,000 UTF-16-unit limit and omitted-unit count. The shared bounded-text operation returns valid UTF-8. A prefix ending with a high surrogate is encoded as U+FFFD, as Node's UTF-8 encoding of the frozen result does.
- Output and file-change sinks own move-only callbacks. Existing environment callers exercise callbacks holding `unique_ptr` state.
- The reviewed commit/completion callbacks use explicit captures and state the lifetime supplied by the blocking completion wait.
- Acquire/release accesses to the execution-completion flag explain the publication relationship and the subsequent join.

The 100 ms grace is an upstream behavior constant, not a locally selected performance threshold. The process test's 1.5-second bound is a liveness backstop that remains enabled on sanitizer lanes.

## Validation

Before the fixes, the public environment regression failed all four scenarios; the isolated closed/failed wait regression failed both paths after the parent killed and reaped its deadlocked child; and the model-request compaction regression failed five Unicode examples while its two ASCII examples passed. These failures distinguish the behavior from the previously passing regression suite.

After the fixes, focused selection `review1044|issue1089|issue1166` passed 12/12 tests. Its four new cases contain four environment scenarios, an established-timeout/late-cancellation discriminator, two wait lifecycle paths and seven compaction examples. The timeout-ordering discriminator also failed before its fix. A final rerun of the four new cases passed 4/4. The seven compaction expectations were independently observed by executing `serializeConversation` from pi-v1.3.0 revision `71023aec1546d6d677a65669c5665c0927ce0dd0` and encoding the result with Node's `Buffer.from`. The ASCII/CJK boundaries use 2,000 and 2,001 characters; the emoji boundaries use 1,000 and 1,001 emoji; the split case uses 1,999 ASCII characters followed by one emoji. This observation is a focused comparison, not a newly registered evidence family.

Full incremental Debug build passed. Both unfiltered runs selected 3,945 CTest cases:

- The default-environment run took 227.75 seconds: 3,937 passed, 1 failed, 7 skipped. Its sole failure was `cch_pi_v130_named_evidence`: the default sibling pi checkout lacked esbuild's platform binary, before any revision check. Selecting the clean frozen checkout explicitly made its 23 Python cases pass.
- The run with `PI_CHECKOUT` explicitly set to the frozen revision took 182.13 seconds: 3,942 passed, 2 failed, 1 skipped. Evidence capture and all six JSONL/SQLite cross-runtime continuation tests passed. The remaining skip was the opt-in Native TUI differential capture.
- The second run's failures were unchanged tests that had passed in the first run: the quarantined projection publication benchmark measured 2.638 ms against its 2 ms processor bound, and the closed-master PTY postdrain test received a different error category. An immediate isolated serial CTest rerun selected exactly those two cases and passed 2/2 in 0.46 seconds. No threshold, exclusion or unrelated production code was changed. These reruns do not turn either recorded full run into an all-green run.

All 43 architecture cases, including standalone Owner Interfaces and the warning gate, passed in both full runs. `scripts/format-check.sh 0f52fcb3acd0a2f20b16ec556f75b92e328d312f` and `git diff --check` passed. Independent Standards and Spec reviews found no actionable issues, including their recheck of the timeout/late-cancellation fix.

## Capability status

No capability membership or implementation-frontier status changes. These fixes strengthen already implemented bounded slices; they do not promote unimplemented inventory rows.

## Remaining boundary

The separate abandon-on-restart capability, recursive watch, shell spill/window retention, other open durable work and the pre-existing unobserved MCP request timeout remain outside this change. No power-loss, fsync-failure, concurrent-writer or full terminal-emulator claim is made.

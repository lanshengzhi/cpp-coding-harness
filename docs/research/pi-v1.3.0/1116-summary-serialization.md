# #1116 summary serialization and transcript preservation

## Scope

B58 / F21c under #1044 and ADR 0075, Named Baseline `pi-v1.3.0`,
`71023aec1546d6d677a65669c5665c0927ce0dd0`, Linux x86-64/glibc.
The owner clarified on 2026-11-01 that the two real summary requests in this
slice are split-turn compaction's **history summary** and **turn-prefix
summary**. Asynchronous branch-summary generation is outside this ticket.
Whole-row comparisons and capability receipts remain owned by #1177.

## Delivered behavior

The first matched C++ observation demonstrated an implementation gap:
`serialize_conversation` included `[Assistant thinking]` and the original
thinking text in summary requests. The repair removes only that serializer's
thinking emission. It preserves assistant text and tool-call serialization;
it does not change message persistence, thinking token estimation or retained
history.

The comparison runs through ordinary `AgentSession` creation/resume,
`compact`, `session_entries` and close/reopen. The existing scripted Models
transport supplies deterministic responses; no live gateway, credentials or
network are used. Two normal prompts persist distinct assistant thinking
blocks. A stored assistant continuation is then appended through the public
SessionStore, and the Session is resumed before compaction. With a one-token
retain budget, the first thinking block falls in the history request and the
second falls in the turn-prefix request. The continuation alone remains in
the retained tail. This fixture makes a fix to only one request path fail.

Independent expected values come from the registered
[audit-1048 capture](../../../fixtures/pi-v1.3.0/audit-1048/session-compaction-boundary.json)
and the supplemental [audit-1116 capture](../../../fixtures/pi-v1.3.0/audit-1116/summary-requests.json).
The latter is a check-validated candidate bundle: its script, manifest, artifact
and exact-revision provenance are committed, while shared-registry registration
and receipt reconciliation remain with #1177. This preserves existing receipt
pins. The candidate runs real frozen `AgentSession.compact` requests over the same
continuation fixture and observes both successful compaction and failure of
the second request after history summarization succeeds.

| Observation | Successful compaction | Turn-prefix failure |
| --- | --- | --- |
| History conversation | `[User]: first request` and `[Assistant]: first answer` | same |
| Turn-prefix conversation | `[User]: second request` and `[Assistant]: second answer` | same |
| Thinking in either summary request | absent | absent |
| Appended transcript entries | one `compaction` | none |
| Original transcript bytes after reopen | unchanged prefix | entire file unchanged |
| Original thinking after reopen | both original blocks | both original blocks |

The C++ assertions compare the complete conversation payload of each request,
the exact combined summary, the exact failure diagnostic, appended checkpoint
kind/count, original binary lines including their newline delimiters, and both
original thinking values returned by the reopened Session. The comparison is
limited to the conversation payload: the existing C++ turn-prefix instruction
wrapper differs from v1.3.0 and is not advanced by this serializer repair.
All discriminating fatal assertions run after Sessions close and the Runtime
workers join. Removing thinking from storage, repairing only one request,
rewriting the transcript on reopen or committing a checkpoint after only the
first request succeeds cannot satisfy the test.

The frozen capture also records branch-summary success/failure as upstream
observations. Ordinary C++ `AgentSession::navigate_tree` currently navigates
without generating an asynchronous branch summary (its existing G2 boundary).
The `SessionTree::branchWithSummary` hook does not establish an application
summary request. No branch-summary parity is claimed here and no new branch
summarizer is added.

## Validation

- Smallest matched observation, before the repair: **1 test case**, with
  normal and second-request-failure sections; **8 failed assertions** showed
  thinking in both history and turn-prefix requests. The probe compiled the
  Session test with the configured flags and linked the existing base archives;
  it was an observation, not repository Full Validation.
- The same probe with the repaired summary object passed **91 assertions in
  1 test case**, including both sections.
- Independent `code-review` agents: **Standards 1 finding, fixed** (non-guarding
  `REQUIRE` changed to `CHECK`); **Spec 0 findings**. No outstanding findings.
- `scripts/format-check.sh fada9afd5d840bb25613b007ba171d12e9e720c8`
  and `git diff --check`: clean.
- `audit_evidence.py check --manifest fixtures/pi-v1.3.0/audit-1116/manifest.json`:
  passed for the supplemental candidate. The registered bundle verifier passed
  with **46 bundles, 102 artifacts and 565 scenarios**. The existing
  `consolidate_audits.py check` passed with **34 delivered rows**,
  **562 consolidated scenarios** and **0 uncovered scenarios**. No existing
  receipt pins or classifications changed.
- Canonical Focused Validation: incremental `cch_tests_coding_agent` build,
  `ctest --preset vcpkg -N -L issue1116` (**1 selected**) and
  `ctest --preset vcpkg -L issue1116 --output-junit /tmp/1116-focused.xml`:
  **1 passed**, covering both sections. The actual
  [JUnit result](deliveries/1116-summary.xml) is retained for #1177. The first
  registered CTest run, with the pre-repair serializer object, failed the same
  eight leak assertions (**91 total assertions, 8 failed**); rebuilding the
  repaired object and relinking made that case pass.
- Full incremental build: `cmake --build --preset vcpkg -j4`: passed.
  The owning shard was first built at `-j2`; the remaining build used `-j4`
  after the other lanes released the shared host budget. The checkout base is
  `fada9afd5d840bb25613b007ba171d12e9e720c8`, plus this ticket's changes.
- Broader compaction selection, after all targets were built:
  `ctest --preset vcpkg -N -L compaction` selected **35**;
  `ctest --preset vcpkg -L compaction --output-junit /tmp/1116-compaction.xml`
  passed **35/35**. The actual
  [JUnit result](deliveries/1116-compaction.xml) is retained.
- Unfiltered Full Validation:
  `TERM_PROGRAM=herdr ctest --preset vcpkg -j2 --output-junit /tmp/1116-full.xml`:
  **4,134 selected, 4,126 passed, 8 skipped, 0 failed**, exit 0,
  **1009.53 seconds**. All **43 architecture checks passed**. The warning
  gate recompiled **620 project compilation units with `-Werror`**, with
  **zero failures and zero compiler warnings**. The eight explicit skips
  require opt-in cross-runtime, realtime-clock or Native TUI differential
  prerequisites; the new regression and all 35 compaction cases ran and passed.
  The full JUnit and transcript are retained locally at `/tmp/1116-full.xml`
  and `/tmp/1116-full-validation.log`. Setting `TERM_PROGRAM=herdr` supplies
  the existing test environment prerequisite; it makes no real-emulator claim.

## Capability status

This slice repairs the measured serializer gap and compares the two selected
compaction requests plus failure/reopen behavior. It does not promote
`cch_coding_agent:Session:compaction:summary-serialization` or edit the
consolidated ledger/frontier/delivery receipts. Supplemental bundle registration
and whole-row reconciliation belong to #1177; historical source-audit statuses
remain historical evidence.

## Remaining boundary

Preserve the ordinary application Session, Magpie-only model access, XDG Pike
state root and strict no-exception behavior. No Durable Runtime changes,
historical Pike import/migration, third-party plugin execution, terminal
emulator evidence or umbrella #1044 completion is claimed.

# #1044 delivery reconciliation — 2026-10-26

## Scope

Reconcile tracker completion with bounded delivery evidence on
`main@72d22b26f20a0685899dc875a3fc6a11208e2ee6`. The owner requested step 1 of the
#1044 completion analysis: correct scope/acceptance bookkeeping, reuse completed
work, and update the frontier. This is documentation/evidence maintenance, not
another feature implementation or acceptance of the complete pi-v1.3.0 target.

The initial GitHub snapshot has 90 direct children: 14 closed audit tickets and
76 implementation slices (42 closed, 34 open). A closed issue is not proof that
its original caller boundary or all comparison obligations have been accepted.
All capability memberships and recorded implementation/verification statuses are
preserved in this reconciliation.

## Delivered behavior

### Restored open obligations

| Ticket | Existing delivery retained | Reason to reopen / remaining acceptance |
| --- | --- | --- |
| [#1095](https://github.com/lanshengzhi/cpp-coding-harness/issues/1095) | Application MCP deferred-tool search through `AgentSession::prompt`, fork and resume; five `[issue1095]` cases in `tests/coding_agent/McpSessionIntegrationTest.cpp` | The issue requires **Durable Harness** tool_search and durable fork/reopen loadouts. Application Session evidence does not exercise that owner. Keep its native blocker #1094 and implement/compare through the durable public caller. |
| [#1093](https://github.com/lanshengzhi/cpp-coding-harness/issues/1093) | Deterministic calls/output, virtual time, seed/fingerprint and replay rejection cases | The delivery comment explicitly leaves OUTCOME-before-settle ordering Deferred because no matching primitive exists. An included operation cannot be waived for that reason. Resolve the public outcome/settlement ordering and record a discriminating comparison; preserve the already delivered replay behavior. |
| [#831](https://github.com/lanshengzhi/cpp-coding-harness/issues/831) | Frozen fitting/over-budget VirtualTerminal comparisons and dated Pike PTY frames | The accepted criterion requires dated terminal A/B growth frames. The close-out records no emulator A/B and an over-budget observation only on VirtualTerminal. Finish the missing terminal comparison; do not restore the rejected footer-layout change. |
| [#1035](https://github.com/lanshengzhi/cpp-coding-harness/issues/1035) | Frozen placement-span/overlay byte comparisons and discrimination probes | `WEZTERM_PANE=1` over a virtual terminal is not a dated Kitty/WezTerm emulator observation. Finish the explicit terminal criterion without discarding the delivered byte contract. |

The original acceptance wording is retained. #831/#1035's terminal checkbox is
unchecked again; the other already accepted criteria remain checked. #1093/#1095
remain unchecked pending their own caller/ordering acceptance. The four tickets
retain their existing native dependencies and regain/retain `ready-for-agent`.
This label describes specification readiness, not the absence of native blockers.

### Closed-ticket acceptance reconciliation

The following mappings justify completing the outstanding **bounded-slice**
checkboxes. They do not promote composite capability rows. Each ticket retains
its existing delivery comment and implementation commit references.

| Ticket | Reviewed acceptance basis | Recorded focused result |
| --- | --- | --- |
| #1061 | Public immutable Tracker snapshots, same-base staleness/revocation, sequential mutable failure and replacement cases; registered audit-1061 observations | 66 cases / 722 assertions |
| #1062 | Backward-gap retention, held observer coalescing versus all 104 fast-observer frames, failing-listener isolation | 68 cases / 1,060 assertions |
| #1063 | Public memory Session/Storage settlement visibility, callback rollback versus storage failure, explicit head/IDs/sequence and close rejection | 16 cases / 415 assertions |
| #1066 | Canonical MCP Owner callers, real headless stdio connection and production CLI failure probe, link/dependency evidence | 268 MCP cases, 43 architecture cases, 1 CLI probe |
| #1067 | Export after destroying the original store, passive snapshot lifetime, JSONL resume and export round-trip, legacy retirement boundary | 12 resume/export cases |
| #1070 | Real resume/tree/fork selectors, disappearance between listing and Enter, removed forwarding surface and architecture gate | 121 gate regressions; recorded label selections in the owning report |
| #1074 | Real JSONL files: confirmed missing sidecar versus torn tail, poison/reopen, sidecar-before-marker, checkpoint/retirement | 6 cases / 280 assertions |
| #1075 | Public SQLite Session/Transaction rollback, sequence preservation, schema rejection, prefix isolation and nondefault options | 10 cases / 158 assertions |
| #1079 | Independent memory Harness workflow and real tools, lazy root/settings/static sections, held tool ordering, final commit failure settlement | 5 cases / 212 assertions |
| #1091 | Public sandbox discovery followed by raw/normalized callable execution, unavailable discovered-name rejection, store/call lifecycle cases | 7 issue-tagged cases |
| #1092 | Ordered mixed outputs/console/image MIME, error taxonomy, memory/output limits and promise-job/unawaited behavior | 7 issue-tagged cases |
| #1096 | Held real worker exit versus settled result, capacity under repeated abort, construction failure and independent joins | 4 cases / 81 assertions |
| #1097 | Inline interrupt budget, cumulative budget across host calls, deadline/abort/pre-aborted behavior | 6 issue-tagged cases |
| #1098 | Real Linux Remote nested exchanges, 5,000-line batching, abort settlement, exit cleanup and transport/cap failures | 9 Remote cases |
| #1100 | Public source parser and declaration tests: line positions/CRLF, typed errors, schema constraints/references/order, output typing and budgets | 6 issue-tagged cases |
| #812 | Recorded duration through wire/projection and live/resumed TUI; changed recorded value changes the screen, no duration before settle; existing working-indicator barrier evidence | 13 cases / 156 assertions |
| #1043 | Four explicitly split families delivered by #1127/#1128/#812/#1129; original parent acceptance is the reconciliation of those families | Child delivery records and NativeTuiDifferentialTest comparisons |

These are historical measured results, not fresh runs in this session. Some
original comments recorded only a build or a failed/filtered full run. For those
tickets, the later unfiltered Full Validation in
[the #1072 record](1072-live-session-consumers.md) supplies the integration result:
3,732 selected, 3,731 passed, 0 failed, 1 prerequisite skip. Git ancestry checks
confirm all implementation commits cited above are included in `6ae4ee6d8`, the
#1072 delivery. This does not erase earlier failures or turn the skipped optional
terminal capture into acceptance evidence.

Historical criteria inside the explicitly superseded sections of #810, #812,
#831, #1033 and #1035 remain unchecked **quoted history**. Prefixing those lines
with `>` makes their existing historical status explicit; it does not tick them
or rewrite the current contract. Closed tickets lose only `needs-*`/`ready-for-*`
state labels; other labels are preserved.

### #1106 reuses #812 and remains open

| #1106 obligation | Existing evidence to reuse | Remaining work |
| --- | --- | --- |
| Slow hook versus short execute; unexecuted calls have no duration | `ToolCallExecutorTest.cpp`: execute-only hook case; blocked call records none; executed success/failure presence | Complete matched frozen outcome coverage, including unknown and length-truncated calls, and a monotonic-versus-wall-clock discriminant. Check the exact execute window rather than accepting the spelling `steady_clock`. |
| Persist/reopen display uses the recording | `SessionMessageJsonTest.cpp`, `ChatContainerTest.cpp`, `InteractiveRenderingGoldenTest.cpp`; #812's live/resume PTY record | Bind the complete comparison scenarios and selected inputs to a delivery receipt. |
| After-hook replacement and nested duration | `ToolCallExecutorTest.cpp`: replacement retains duration; `NestedToolCallsTest.cpp`: end event equals recorded duration | Reuse these cases; assess any remaining frozen error/cancellation paths at the public caller. |
| Focused/Full Validation and status update | #812's recorded selections and later #1072 integration run | Run the selection for the actual #1106 changes and advance only completely evidenced rows. |

No #1106 criterion is automatically accepted in this documentation pass. The
issue is annotated with this map so its implementer starts with existing behavior
and finishes the missing comparisons instead of rebuilding propagation.

### #1072 evidence registration and existing receipt refresh

Registered the unchanged `audit-1072/manifest.json` and `live-session.json` through
`audit_evidence.py register`. Its two observations are explicit orphans in the
consolidation plan: selected JSONL branch versus all-entry HTML/selected leaf, and
SessionInfo append parent/leaf behavior. The latter drives SessionManager
itself; it does not claim frozen interactive-selector equivalence.

The only changed input among the 51 pins of the #1101 receipt is the consolidation
plan. Its delta adds those two orphan records only. All old mappings, the MCP row,
its exact frozen scenarios, production sources, tests and expected bytes are
unchanged. After rebuilding both comparison targets, 14/14 #1101 cases passed in
[the new JUnit artifact](deliveries/1044-reconciliation-1101.xml). The receipt
retains its implementation commits, row/scenario mapping and discriminating case,
refreshing the plan pin and actual validation record. Previous JUnit artifacts
remain available; no new row is promoted and no verifier is relaxed.

The JUnit timestamp is local `2026-10-26T15:50:43` (UTC+09:00); the receipt records
`2026-10-26T06:50:43Z`. Its `testedRevision` names the checkout base and the pins
bind the selected input bytes, including the reviewed plan addition.

## Validation

- Incremental `cch_tests_coding_agent` and `cch_tests_mcp` build: passed.
- Exact `ctest --preset vcpkg -N -L issue1101` selection: 14 cases.
- `ctest --preset vcpkg -L issue1101 --output-junit <repository>/docs/research/pi-v1.3.0/deliveries/1044-reconciliation-1101.xml --output-on-failure`: 14 passed, 0 failed, 0 skipped.
- `consolidate_audits.py build` and `check`: passed; 1,464 rows, one validated delivery row, 477 non-bootstrap scenarios, 75 explicit orphans and zero uncovered scenarios.
- `python3 tests/evidence/ConsolidationAuditTest.py`: 39 passed.
- `python3 tests/evidence/NamedEvidenceTest.py`: 18 passed, zero skips.
- `audit_evidence.py verify`: 27 bundles, 81 artifacts and 480 scenarios at the full frozen revision.
- `scripts/verify-closed-issue.sh` against every currently closed direct child: **52 selected, 52 passed**. The final tracker snapshot has 52 closed and 38 open direct children: 14 closed audits and 38 closed / 38 open implementation slices. #1126 has 25 open direct blockers; 21 open implementation tickets have no native blockers.
- Removed stale state labels from 26 closed tickets. Corrected active acceptance checklists on 17 bounded deliveries, preserved five superseded historical checklists as quoted history, and restored the four open obligations above. No closed child retains an unchecked active checklist or in-flight label.
- Compared the initial/final native dependency totals for all 90 direct children: no edges added or removed. Reopening the four tickets naturally restores their blocking effect.
- Compared the complete generated row objects and aliases against the checkout base: all 1,464 rows and aliases are unchanged. The plan delta is exactly two new audit-1072 orphan records.
- `scripts/format-check.sh 72d22b26f20a0685899dc875a3fc6a11208e2ee6`, `git diff --check`, and relative-link checks: passed.
- Manual review of the documentation/evidence delta confirmed preserved frozen bytes, unchanged MCP scenario mapping, accurate caller distinctions and explicit remaining work.

No production/test source or build contract changes. The documentation/evidence
validation tier applies; the focused MCP run refreshes the affected receipt.
This session does not claim a new unfiltered C++ Full Validation or terminal run.

## Capability status

All 1,464 row identities, source rows, memberships and implementation/verification
statuses are preserved. Exactly one existing row remains verified through the
refreshed #1101 receipt. #1072 adds registered observations, not a new verified row.
The current Owner frontier now records #1068–#1073 and bounded #1079 as delivered,
and distinguishes #812's duration implementation from #1106's remaining acceptance.

## Remaining boundary

#1044 and #1126 remain open. Native GitHub dependencies remain the live scheduling
authority; the 32 original drafts are historical planning records. Finish #1077,
#1078, #1080 and #1081/#1082 before the dependent crash/interchange acceptance.
The reopened #1093/#1095/#831/#1035 obligations are explicit work, not a Deferred
exemption or work silently assigned to final integration. Whole-row comparisons,
real emulator evidence and full Durable Runtime remain outstanding. Magpie/XDG,
the 29 exclusions and the no-historical-Pike-migration boundary are unchanged.

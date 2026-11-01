# #1108 Agent defaults, busy/reset and settled cancellation

**Status:** implemented and focused-verified against the frozen baseline. No capability row is
promoted here: whole-row Agent and application receipts are owned by #1176 and #1177 (issue
reassessment of 2026-10-30), and this report is the comparison evidence those receipts consume.

- **Baseline:** `pi-v1.3.0` at `71023aec1546d6d677a65669c5665c0927ce0dd0`
- **Capture host:** Linux x86-64, glibc 2.44, Node v26.13.0, tsx 4.22.1
- **Frozen checkout:** `/tmp/pi-src` (clean; `HEAD` equal to the baseline)
- **Integration base:** `0087b09fc7ae8990d505ec4c5fe13498e09e0fc2`
- **Evidence family:** `fixtures/pi-v1.3.0/audit-1108/` — two artifacts, five scenarios
- **Batch:** B50 / AReset (parent #1044, ADR 0075, accepted #1058 consolidation)

## Evidence bundle

| Artifact | Scenarios | SHA-256 |
|---|---:|---|
| `reset-lifecycle.json` | 3 | `c9a3b57e44c85c200ffed370d3bda87f08ea2ea92a29feea1444d6a81a3942e1` |
| `settled-cancellation.json` | 2 | `1bfb5d8b1206a608f4a9ef40e140c7e0b19f3d5eb4b683d8dbce4d2018d966fe` |

`manifest.json` SHA-256: `39a10e2dc3f2e7b71588a62e9eed671f614a743976df7537676f8b8cb8f9cb4b`.
Capture generator SHA-256: `1aef4d15e156705c40b2dcb4447d57a7727dd52f82bf44997fca2e110a9fc9e7`.
The bundle is a candidate, not a registered family: serial registration in
`fixtures/pi-v1.3.0/baselines.json` is owned by the merger (#1176), matching the lane precedent of
#1047. `python3 scripts/evidence/audit_evidence.py check --manifest
fixtures/pi-v1.3.0/audit-1108/manifest.json` accepted it and returned the registry record:

```json
{
  "audit-1108": {
    "manifest": "audit-1108/manifest.json",
    "manifestSha256": "39a10e2dc3f2e7b71588a62e9eed671f614a743976df7537676f8b8cb8f9cb4b"
  }
}
```

```bash
PI_CHECKOUT=/tmp/pi-src PI_BASELINE=pi-v1.3.0 \
  /tmp/pi-src/node_modules/.bin/tsx fixtures/pi-v1.3.0/audit-1108/capture-1108.mts
# captured audit-1108 (2 artifacts, 5 scenarios) for pi-v1.3.0 (71023aec…)
```

`reset-lifecycle.json` drives the real public `Agent` with a scripted `streamFn`, a real tool, a
steering message and a follow-up message: it records the post-`reset` message roles, the replayed
System Message content, sections and tool declarations, the rendered `state.systemPrompt`, the
retained model and thinking level, the cleared queues and the exact busy diagnostic raised while a
run is active. `settled-cancellation.json`
records `ProgramStatusReporter` as a **source probe**: the reporter consumes loop events and writes
the OSC 7501 terminal frame, so the observed `state=idle` / `state=done` bytes are the behavior
under test and no provider or gateway is involved. The bundle therefore mixes an executable Agent
capture with a recorded reporter probe; the report names which scenario is which.

## Implementation

- **Agent default thinking level.** `AgentInitialState::thinking_level` is empty by default and an
  empty value requests the pi Agent default `off` (`createMutableAgentState`'s
  `initialState?.thinkingLevel ?? "off"`). `AsyncAgentOptions::thinking_level` is applied only when
  the initial state does not name a level. The application Session still passes its resolved level
  (`AgentSessionExecution.cpp`), so the v1.3.0 `medium` session chain is unchanged.
- **Reset.** `Agent::reset()` rejects while a run is active with pi's exact text, otherwise replays
  the transcript's System Messages into one baseline through `current_system_message` (pi
  `getCurrentSystemMessage`), restores the live System Prompt from it as the rendered content plus
  retained section text (pi `getCurrentSystemPrompt`), clears steering/follow-up queues and
  transient run facts, and retains tools, Model and thinking level.
- **Busy diagnostics.** `prompt`, `continue` and `reset` each report pi's exact per-operation text on
  the Agent; the application Session's streaming rejection reports pi's `AgentSession.prompt` text
  (`Specify streamingBehavior ('steer' or 'followUp') ...`) instead of the legacy busy string.
- **Settled cancellation.** The native TUI evidence case now checks the interrupted run's
  `agent_settled` mapping to idle, its aborted stop reason, `is_busy()` returning false, and the
  next run mapping to done with its own final text.

## Acceptance and separating cases

| Acceptance | Normal case | Separating case |
|---|---|---|
| Default `off` is the Agent default, not a clamp | `the Agent default thinking level is off for a reasoning-capable model`: a model supporting every level still reports `off` and sends `reasoning == nullopt` | The same test constructs the same model with an explicit `"medium"` initial level and requires it to stay `medium`. A stand-in that defaults to `medium` and clamps per model passes the first check for non-reasoning models but fails this one; `reasoning == nullopt` also separates "off" from "clamped to off" on the wire. |
| Reset restores the baseline and clears queues | `reset restores the System Message baseline and clears queues while retaining live policy`: after a tool run with a steering and a follow-up message queued, reset leaves `["system"]` with the replayed text plus `echo` declaration, empty queues, and the retained Model/thinking level; the next request carries that baseline. `reset renders replayed System Message sections into the live prompt` covers the section half of the replay: the baseline keeps `alpha`/`beta` and `state.systemPrompt` becomes `"base\n\nA\n\nB"` | `reset during an active run is rejected without changing the run or transcript`: reset is refused with the pi text while the gated run is active and the state still holds exactly the user message. A stand-in that resets unconditionally passes the state checks after a completed run but fails this refusal. |
| Busy does not replace the current run | `stateful Agent rejects a second prompt while its active run is suspended`: exact prompt text, `request_count == 1`, the original run finishes with its own `"released"` answer | The same case's request count and final transcript separate a stand-in that reports the text but replaces or aborts the run; the reset refusal above is the independent busy path. |
| Application busy diagnostic and run identity | `a second session prompt is rejected with pi's application text without replacing the run`: a real `AgentSession` with a gated provider rejects the second prompt with pi's application string, keeps one provider request and a two-message transcript, then completes the first run with `"turn 1"` | A stand-in that keeps the legacy `"session is busy (prompt already in flight)"` text fails the exact comparison; one that queues or replaces the second prompt fails the request-count and `last_assistant_text` assertions. |
| `continue` and `reset` busy diagnostics | `AgentBehaviorTest` continue-busy expectation; reset refusal message | The reset refusal is the separating case for the reset text: a shared "already processing" string would fail the exact comparison while still reading as busy. |
| Real application cancellation settles idle | `Native TUI returns to idle when a run is interrupted, then reports the next run`: the cancelled run's settled report is idle with an aborted stop reason and the session is not busy; the next run reports done with `"after cancel"` | A reporter that only ever emits done, or that leaves the busy flag set, passes a "state changed" check but fails the idle+aborted pairing or the `is_busy()` assertion. |

## Focused Validation

Built the owning shards with the pinned vcpkg preset (`cch_tests_agent`,
`cch_tests_coding_agent_interactive`) and selected by label/name:

| Selection | Selected | Result |
|---|---:|---|
| `ctest --preset vcpkg -L issue1108` | 7 | 7 passed |
| `ctest --preset vcpkg -L '^agent$' -j2` | 236 | 236 passed (an earlier run under a concurrent full build had one load-sensitive failure; the final Full Validation is the authority) |
| `ctest --preset vcpkg -L '^architecture$' -j2` | 43 | 43 passed |
| `ctest --preset vcpkg -L '^(issue1090|issue1101|issue1104)$'` (receipt 1104 revalidation) | 36 | 36 passed |
| `ctest --preset vcpkg -L issue1118` (receipt 1118 revalidation) | 19 | 19 passed |

The same three selections were re-run on the tree merged with `origin/main` (`6764d6bcf`, #1116 /
#1117 / #1170): 7/7, 236/236 and 43/43 again.

The Parity Architecture Gate passed during configure and every build phase (include-evidence
re-derived after the `ToolLoadout` include change). Formatting: `scripts/format-check.sh
0087b09fc7ae8990d505ec4c5fe13498e09e0fc2` reports `format check: clean`.

## Pinned-receipt revalidation

`src/coding_agent/AgentSessionExecution.cpp` is a pinned input of the #1104 and #1118 delivery
receipts, and the application busy text moved its digest. Following the reconciliation contract, only
that digest and the validation records moved: both receipts keep their rows, scenario mappings and
`routingSha256`, retain the previous validation block under `revalidation1108.previousValidation`,
and gained a fresh JUnit artifact (`1104-revalidation-1108.xml` 36/36,
`1118-revalidation-1108.xml` 19/19). The change makes no new verification claim about those rows.

## Full Validation

`cmake --build --preset vcpkg` completed, then the unfiltered suite ran serially twice:

1. First run: 4133 selected, 4127 passed, 6 failed, 8 skipped. Two failures were real and the
   receipt revalidation above fixes them (`#4130`/`#4131` rejected the stale
   `AgentSessionExecution.cpp` digest). The other four were load-sensitive and each passed when run
   alone: `#916` (issue1019 postdrain EIO), `#1890` (codemode worker settle), `#2191` (the #632
   quarantine projection cost bound, normally excluded by CI's `-LE quarantine`) and `#3275` (MCP
   e2e menu).
2. Final run with the recorded `TERM_PROGRAM=herdr` precondition: 4133 selected, **4133 passed, 0
   failed, 8 skipped** in 574.61 s.

After merging `origin/main` (`6764d6bcf`, #1116/#1117/#1170) the full preset rebuilt and the same
unfiltered serial suite selected 4147 cases: **4147 passed, 0 failed, 8 skipped** in 810.97 s. Two
intermediate merged-tree runs under a concurrent external `-j4` build each had load-sensitive
failures (#411, #2198, #2200, #1189); every one passed when rerun, and the final run above is clean.

The skipped cases are the suite's standing preconditions (six frozen-pi/durable integration cases,
the realtime-clock case, and the native TUI differential capture), not #1108 selections.

## Capability status

No consolidated ledger row changes in this ticket. The issue reassessment assigns whole-row Agent
and application receipt ownership to #1176 and #1177; this report supplies the comparisons those
receipts need for `cch_agent_core:Agent:default-state`,
`cch_agent_core:Agent:prompt-busy` and `cch_agent_core:Agent:reset`. `Agent:default-state` also
carries the older `toolExecution`/`transport` surface mapping recorded in #1047 (fixed-per-adapter
transport; per-tool `ToolConcurrency` instead of pi's `toolExecution` option), which stays in that
row's notes rather than being promoted away by this slice.

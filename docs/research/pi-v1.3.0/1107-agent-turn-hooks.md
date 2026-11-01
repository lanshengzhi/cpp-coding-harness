# #1107 Agent prepare/finish turn hooks delivery

## Scope

Batch B49 / ALoop, under #1044 and ADR 0075. Named Baseline: `pi-v1.3.0`,
revision `71023aec1546d6d677a65669c5665c0927ce0dd0`, Linux x86-64/glibc.

This slice aligns the public `cch_agent_core` Agent loop policies with pi
v1.3.0's `prepareRequest`, `prepareNextTurn` message appending, and
`finishTurn` end/continue decisions, and keeps the application's between-turn
compaction caller correct.

Frozen evidence: the registered `fixtures/pi-v1.3.0/audit-1047/lifecycle.json`
scenarios `prepareRequest observes the pending prompt before the provider
request`, `prepareNextTurn runs once before a continuing turn and can append
messages`, and `finishTurn action end stops the run before follow-up polling`;
plus the new candidate family `fixtures/pi-v1.3.0/audit-1107/turn-hooks.json`
(9 scenarios, captured with `capture-turn-hooks.mts` at the named revision,
manifest SHA-256 `07f8771d760be638755c04a3e9bae1a0c7216cde2124959b5120bde9ec3b83d1`,
`audit_evidence.py check` passed). Registering `audit-1107` in
`baselines.json` and creating its whole-row delivery receipt are owned by
#1176/#1183, as in the #1106 precedent. The #1104/#1118 input re-pins required
by this slice are recorded as `revalidation1107` entries.

The #1058 F17 observations listed in the issue (`before_agent_start` busy state
and contribution, extension-triggered run retention) describe the application
Session's extension lifecycle, which this slice does not touch: there is no
extension caller in `cch_agent_core` and the application extension runner is a
separate slice. The loop observations above are the matching evidence for this
capability.

## Delivered behavior

- **`prepareRequest` (pi `AgentOptions.prepareRequest`).** New
  `AsyncAgentOptions::prepare_request` / `PrepareRequestHook` receives
  `PrepareRequestContext{context, model, thinking_level}` after the pending
  messages have been appended and emitted, immediately before every provider
  request including the first. It returns `AgentRequestUpdate` (pi
  `AgentRequestUpdate`: context replacement, model, thinking level) whose
  values replace the run's runtime state for that request and later requests.
  It never polls either input queue. A model change still requires
  `validate_turn_update`; a hook failure ends the run before the request.
- **`finishTurn` (pi `AgentOptions.finishTurn`).** New
  `AsyncAgentOptions::finish_turn` / `FinishTurnHook` receives the completed
  turn's `AgentTurnContext` after the assistant and tool-result messages and
  **before** `TurnEndEvent`. `AgentTurnAction::End` stops the run without
  polling either input queue or invoking prepare-next-turn;
  `AgentTurnAction::Continue` guarantees one further provider request, which a
  natural tool-result, steering, or follow-up request satisfies without an
  extra one. An error or aborted assistant still invokes the hook before
  `TurnEndEvent`, and its decision cannot extend the hard exit. The pi-0.85.1
  `should_stop_after_turn` seam is retired; ADR 0014 records the addendum.
- **Appendable `prepareNextTurn` (pi `AgentLoopTurnUpdate.messages`).**
  `AgentLoopTurnUpdate` gains `messages`. The loop splits them from the request
  fields, emits each through the ordinary message lifecycle after
  `turn_start`, and places them ahead of steering queued while the hook ran
  (pi `[...preparedMessages, ...pendingMessages]`). They enter the next request
  and the invocation's `agent_end` slice.
- **Completed-turn vocabulary.** `PrepareNextTurnContext` is renamed
  `AgentTurnContext` (pi `AgentTurnContext`) and is shared by `finishTurn` and
  `prepareNextTurn`. `AgentRequestUpdate` carries the request-relevant fields,
  and `ValidateTurnUpdateHook` validates those fields (model, thinking level,
  context) rather than appended messages.
- **Application compaction caller.** `AgentSession::Impl::compact_before_next_assistant_response`
  keeps its context-replacement return through the request/update split: the
  between-turn trigger still fires only when the loop will continue, and its
  rebuilt context reaches the next request.

Comparisons (frozen scenario → C++ case, all in
`tests/agent/AgentBehaviorTest.cpp` unless noted):

| Frozen observation | C++ comparison |
| --- | --- |
| `audit-1107`: prepareRequest observes the emitted pending prompt and can replace the request state | `prepareRequest observes the emitted pending prompt and replaces the request state` |
| `audit-1107`: prepareRequest replacement state carries into the next request of the run | `prepareRequest replacement state carries into the next request of the run` |
| `audit-1107`: prepareRequest does not poll the steering queue | `prepareRequest does not poll the steering queue` |
| `audit-1107`: finishTurn runs after the tool-result message and before turn_end | `finishTurn runs after the tool-result message and before turn_end` |
| `audit-1047`: finishTurn action end stops the run before follow-up polling | `finishTurn end stops before queue polling and next-turn preparation` |
| `audit-1107`: finishTurn action continue makes exactly one context-only request | `finishTurn continue makes exactly one context-only request` |
| `audit-1107`: finishTurn action continue is satisfied by the natural tool-result request | `finishTurn continue is satisfied by the natural tool-result request` |
| `audit-1107`: finishTurn observes an aborted assistant before turn_end without continuing | `finishTurn observes an aborted assistant before turn_end without continuing` |
| `audit-1107`: prepareNextTurn appended messages are emitted before the next request | `prepareNextTurn appended messages are emitted before the next request` |
| `audit-1107`: prepareNextTurn replacement context and appended messages both reach the next request | `prepareNextTurn replacement context and appended messages both reach the next request` |

The discriminating cases are the ordering and merge cases. `finishTurn runs
after the tool-result message and before turn_end` fails for a hook invoked
after `turn_end` or before the tool-result `message_end`; a check that only
counts hook calls passes in both broken shapes. `prepareNextTurn replacement
context and appended messages both reach the next request` fails when the
appended messages are merged before the replacement (they are dropped) or
ignored; asserting the second request's user texts separates the merge order.
`prepareRequest does not poll the steering queue` separates a hook that drains
the queue itself: the late steering message must be absent from the first
request and present in the second.

Failure and cancellation: `finishTurn failure ends the run before turn_end`,
`prepareRequest failure ends the run before the provider request`,
`prepareRequest model replacement without a validator is rejected`, and
`finishTurn observes an aborted assistant before turn_end without continuing`
pin the failure channels and the hard exit.

The application caller is verified by the existing
`tests/coding_agent/AgentSessionCompactionTest.cpp` cases `between-turn
threshold compaction replaces the run context before the next response` and
`the final turn skips the between-turn trigger; window-exceeding usage reports
the post-run overflow reason`; both run through `AgentSession::prompt`.

## Validation

- Focused Validation, `ctest --preset vcpkg -R 'prepareRequest|finishTurn|prepareNextTurn'`:
  22 selected, 22 passed (includes the pre-existing prepareNextTurn cases).
  The `issue1107` label selects the 13 new cases: 13 passed.
- Application compaction caller, `ctest --preset vcpkg -L compaction`:
  34 selected, 34 passed after the full build registered every compaction case.
- Owning module label, `ctest --preset vcpkg -L '^agent$' -j2`: 231 selected,
  231 passed (the realtime-clock wrapper is skipped in a shard-only selection
  and runs in the full suite). One earlier shard-only run hit the pre-existing
  runner-sensitive `bounded parallel cancellation leaves queued tool
  capabilities unstarted` case (issue #497), which reproduced on `main` at the
  same base commit and passed on isolated rerun.
- Architecture: the full run includes 43 architecture-labelled entries and
  none failed; the separate `ctest --preset vcpkg -L architecture -j2`
  selection passed 36/36 at the shard-build stage and 43/43 after the full
  build.
- Full Validation on the reviewed tree, `cmake --build --preset vcpkg` then
  `ctest --preset vcpkg -j1`: 4,115 selected, 4,113 passed, 2 failed,
  8 skipped. Both failures are the documented `[quarantine]` performance cases
  from #632 (`Projection publishes 100 message-update chunks…` measured
  2.19 ms processor against its 2 ms Debug bound with all 172 functional
  assertions passing; `Streaming assistant incremental block freeze…`), which
  the CI lanes exclude. The Projection case also fails intermittently on the
  unmodified primary checkout at the same base commit (measured 2.038 ms in
  one of three isolated runs), and both passed on isolated rerun here. No
  functional case failed. Two earlier runs under concurrent lanes (load
  average 12-17) additionally flaked `closing codemode settles the run…`,
  `/mcp reconnect with no name…`, two Process Terminal cases, a durable
  generation case, and a cancellation case; each passed in isolation and none
  touches the Agent loop. The two review-fix lines made after that run (default
  member initializers on the turn state and a const copy instead of a moved
  one) are behavior-neutral and were revalidated with the agent module label
  (231 passed), the compaction label (34 passed), and the issue1107 selection
  (13 passed).
- Formatting: `scripts/format-check.sh origin/main` clean.
- Evidence: `audit_evidence.py check --manifest
  fixtures/pi-v1.3.0/audit-1107/manifest.json` passed.
- Revalidation: this slice changes `src/coding_agent/AgentSessionExecution.cpp`,
  a pinned input of the #1104 and #1118 delivery receipts. Their selections
  were rerun on the final tree into fresh JUnit artifacts (36 and 19 selected,
  all passed) and each record carries a `revalidation1107` entry with refreshed
  input and artifact digests; no row, scenario, or routing claim changes.
- Consolidation: `consolidate_audits.py build` and `check` passed; the
  consolidation boundary tests passed (42).
- Two-axis review. Standards: fixed the Owner-Interface default member
  initializers on `PrepareRequestContext` (§4.1); replaced the four-reference
  state fold with a `TurnRequestState` value fold that returns the updated
  state (§13.2, and the reported parameter-clump smell); and extracted the
  duplicated `finishTurn` dispatch into `Impl::invoke_finish_turn`. Rejected:
  the reported duplicate of `AgentCoreEvidenceTest.cpp`'s `message_role`
  helper — that helper uses a different, narrower vocabulary
  (`tool_result`/`other`), and no shared `tests/support` role helper exists;
  the test-local visit is the direct form for the vocabulary it asserts. Spec:
  no findings; the reviewer traced all ten frozen scenarios and the
  application compaction caller through the implementation.

## Capability status

No capability row is promoted. `cch_agent_core:Agent:prepare-request-hook`
stays `upstream-captured` / `missing`, and
`cch_agent_core:Agent:finish-turn-hook` and
`cch_agent_core:Agent:prepare-next-turn-hook` stay `upstream-captured` /
`partial` in the consolidated ledger: this slice delivers the implementation
and its bounded comparisons, while whole-row promotion, receipt re-pin, and
the `audit-1107` registration belong to #1176 (Agent verification) and #1183
(consolidation index).

## Remaining boundary

- `prepareRequest` and `finishTurn` have no application caller yet. pi's
  application installs `prepareRequest` for the session projection and virtual
  model routing, and `finishTurn` for the extension `turn_end` boundary; those
  are separate slices, so this delivery verifies the hooks through the public
  Agent only.
- Loop policy hooks do not receive the run stop token (the pre-existing
  `prepareNextTurn` shape); pi passes its abort signal to all three. Run-level
  cancellation still ends the run through the ordinary aborted terminal.
- `AgentRequestUpdate` model/thinking replacements persist in live Agent state
  after the run, the established C++ turn-update policy; pi keeps them
  loop-local. The request-visible behavior for this and later requests in the
  run matches the frozen observation.
- `audit-1107` is an unregistered candidate bundle; registration and receipt
  re-pin are #1176/#1183 work.
- #1044 remains open.

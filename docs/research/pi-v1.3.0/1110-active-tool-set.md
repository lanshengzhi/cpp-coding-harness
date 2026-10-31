# #1110 active tool set execution and tool-change declarations

## Scope

Batch B52 / F16, under #1044 and ADR 0075. Named Baseline: `pi-v1.3.0`, revision
`71023aec1546d6d677a65669c5665c0927ce0dd0`, Linux x86-64/glibc.

Frozen evidence: `fixtures/pi-v1.3.0/audit-1047/tool-execution.json`, scenario
`replacing state.tools narrows the executable set and declares the system tool
delta` (inputs `initialTools: ["a","b"]`, `replacementTools: ["b"]`,
`issuedCall: "a"`). Owner row: `cch_agent_core:ToolRegistry:active-set`.

## Delivered behavior

- **`context.tools` is the executable set.** `ToolCallExecutor` resolves a
  model-issued call only against `request.context.tools` — the run's executable
  set, pi `currentContext.tools` and `prepareToolCall`. A registered tool the run
  did not declare is rejected as unknown, never executes, and has no duration.
  Sequential-batch detection is gated on the same set, so a non-executable
  exclusive tool cannot serialize a batch it is not part of. An empty executable
  set therefore executes nothing (fail-closed).
- **The nested seam keeps the registry.** `execute_nested` still hands
  `RunToolCallOptions{registry_}` to the nested path (pi `_getCallableTools`), so
  a codemode script or `tool_search` reaches a registered tool the run never
  declared. Routing nested resolution through `context.tools` would remove tool
  discovery the moment a loadout narrows; `a script reaches a registered tool
  the run never declared` fails if that happens.
- **Tool-change declarations.** New owner-interface module
  `<cch/agent/ToolLoadout.hpp>` ports pi `utils/transcript.ts` and
  `agent-loop.ts` `declareToolChanges`: `tool_declaration` (pi
  `toToolDeclaration`, which drops `outputSchema`), `declarations_equal`,
  `tool_state_changes` (pi `getToolStateChanges`), `current_tools` (pi
  `getCurrentTools`) and `declare_tool_changes`. Before every request the delta
  between the transcript's declared tool state and the executable set becomes a
  system message: inserted before the first non-system pending message, or
  patched into the pending system prompt-section message that anchors it.
  Additions follow the executable set's order; removals follow the previous
  declaration's order; a changed definition is a removal plus an addition, so
  replaying the delta always yields exactly the executable set.
- **Every prompt declares its section diff on the same entry.** pi
  `_prepareRun` → `_preparePromptAndToolLoadout`
  (`core/agent-session.ts:1743-1754`) diffs the prompt sections against what the
  live transcript replays, unshifts the resulting system message ahead of the
  prompt's own messages, and `declareToolChanges` then anchors the tool delta on
  that message. The port had sections only at Session construction and on
  `/reload`, so a resumed session declared a tools-only entry. New:
  `AgentSession::Impl::pending_prompt_sections` (empty when nothing changed, so
  no section is ever declared without a difference) and
  `Agent::prompt_with_prefix` (private, through `AgentPromptAccess`) carrying
  `InitialInput::prefix_messages`, committed before the run's user message.
- **A bare Agent seeds its declared loadout; a Session does not.** `Agent::Impl`
  seeds the leading system message from the system prompt and initial
  declarations (pi `createMutableAgentState` → `createInitialSystemMessage`) when
  `messages[0]` is not already a system message and
  `AsyncAgentOptions::seed_initial_system_message` is set — the pi bare-Agent
  default, which the frozen observation requires (its `agent_end.messages` of 5
  against `totalMessages` of 6 is exactly one seeded message outside the run).
  A Session turns the seed off, as pi's `sdk.ts` does by constructing its Agent
  with `systemPrompt: ""` and `tools: []`: the recorded transcript is never
  rewritten, and `declareToolChanges` announces the loadout as a delta before the
  first request. A fresh session already leads with the System Message the
  Session builds, and that message now declares the loadout the Agent actually
  starts with — every initially active registered tool, built-ins in pi's
  registration order — instead of only the prompt-selected subset, so the first
  request carries no spurious delta.
- **The declared set keeps the caller's order.** `snapshot()` builds
  `state.active_tool_names` in the order the caller named it (pi `AgentState.tools`)
  rather than registry order, and `set_active_tools` dedupes like pi
  `[...new Set(toolNames)]` while copying the caller's array.

Against the frozen observation: `assignedTools ["b"]`, `executed {a:0,b:0}`,
`systemDeclarations [added:[a,b] removed:[]], [added:[] removed:[a]]`,
`totalMessages 6`, `agent_end.messages 5`, and `durationPresent false` all match.
`resultText` does **not** match: pi emits `Tool a not found`, while the port keeps
the bounded diagnostic `unknown tool: a (argument location: root)`. That is the
tracked diagnostics divergence in `1047-capability-ledger.json` (audit gap #10),
not an accepted equivalence.

## Comparisons

| Frozen observation | C++ comparison |
| --- | --- |
| replacing `state.tools` narrows the executable set and declares the system tool delta | `tests/agent/AgentToolSurfaceTest.cpp` — `a removed active tool never executes and the request declares the ordered delta`; `tests/agent/ToolLoadoutTest.cpp` — `tool_state_changes reports additions in current order and removals in previous order`, `declare_tool_changes inserts the delta before the first non-system message` |

`a resumed session declares its loadout without rewriting recorded history`
(`tests/coding_agent/AgentSessionSnapshotTest.cpp`) separates the ownership rule:
before any prompt the resumed snapshot is the recorded message list verbatim, and
after one prompt the recorded message keeps position 0 while one entry follows it
carrying both the full section diff (`preamble` first, no removals) and
`toolsAdded` equal to the non-empty executable set in order. Re-enabling the
constructor seed fails both halves, and a non-empty check keeps the tool half
from passing vacuously. `a prompt declares only the section diff and the loadout
delta it needs` covers the existing-sections case: a stale recorded `preamble`
is replaced, no removal is invented, and a second prompt that changes nothing
declares nothing. Its recorded transcript declares a system message without
tools, so the restored loadout is empty (pi `_restoreToolsFromTranscript`) and
there is no tool delta to merge — the one-entry rule is asserted in the case
above.

The discriminating case is `a removed active tool never executes and the request
declares the ordered delta`. It drives the public `Agent` seam only
(`set_active_tools`, then `prompt`) with a counting tool: the removed name's
call must be an error result with no duration while `executes["alpha"] == 0`,
and — the separation the execute count alone would miss — a later prompt that
calls the still-active `beta` must execute it exactly once. A harness that never
runs the counter, or one that still resolves calls against the registry, cannot
satisfy both halves. `a script reaches a registered tool the run never declared`
separates the nested capability from that rejection: with `context.tools` limited
to `{"codemode"}`, the script's `tools.echo` call still runs.

Collateral: `state().messages.size()` assertions in cases that register tools
shifted by one, because the leading system message is now seeded. Each was
re-derived against pi's transcript shape rather than adjusted to fit: the seeded
message is the run's leading system message, so the user message moves from
index 0 to 1 and `agent_end.messages` gains it.

## Validation

- Focused Validation, `ctest --preset vcpkg -L issue1110`: 13 selected, 13 passed,
  0 failed, 0 skipped. `ctest --preset vcpkg -L tool-loadout`: the same 13.
- Owning module label, `ctest --preset vcpkg -L '^agent$' -j2`: 218 selected, 218
  passed. The realtime-clock case is skipped in the shard-only selection and is
  covered by its `cch_agent_realtime_jump` wrapper inside that label.
- Codemode routing, `ctest --preset vcpkg -R codemode -j2`: 91 selected, 91 passed.
- Architecture, `ctest --preset vcpkg -L architecture -j2`: 43 selected, 43
  passed, including `cch_owner_interface_standalone` compiling the new
  `<cch/agent/ToolLoadout.hpp>`. The build-phase Parity Architecture Gate passed
  after reconfiguring for the changed include sets (`PARITY-4011`).
- Full Validation on the branch after merging `origin/main` (`766f9a477`, which
  brings the #1174 durable JSONL fsync work; no overlap with this slice), with
  `TERM_PROGRAM=herdr` and no other suite running: `cmake --build --preset vcpkg`
  then `ctest --preset vcpkg -j2` — 4,078 selected, 4,078 passed, 0 failed, 8
  skipped, exit 0. The 8 skips are the six frozen-pi durable continuation cases,
  the realtime-clock wrapper and the Native TUI differential capture, none of
  which this slice touches. An earlier run of the same suite under a concurrent
  full suite in worktree `1174` (load average ≈ 8) reported six failures; all
  five non-environmental ones passed on isolated rerun, which is why the record
  above is the unloaded run and not that one.
- Formatting: `scripts/format-check.sh origin/main` clean.
- Consolidation: `consolidate_audits.py build` wrote 1,464 rows; `check` passed
  (7 delivered rows, 562 scenarios, 144 explicit orphans, 0 uncovered) and both
  consolidation boundary tests passed. `AgentSessionExecution.cpp` is a pinned
  input of receipt `1104-management-during-connection`, so its shared 36-case
  selection was rerun into `deliveries/1104-revalidation-1110.xml` (36 selected,
  36 passed) and the record carries a `revalidation1110` entry; no other input was
  rehashed and that receipt makes no claim about this row.
- PM review (second axis, after delivery): keep the Session's constructor seed
  disabled — accepted as implemented; verify and complete the resumed prompt's
  section diff at the Session seam — implemented as described above, and the
  `ToolCallBatchRequest` audit found no production caller beyond `AgentRun.cpp`;
  add the frozen event/duration assertions to the removal case — added (single
  start/end pair, `is_error`, no duration on event or result, 1-then-0 tool
  results per turn, `agent_end` slice of 5), with the unknown-tool text pinned as
  the tracked divergence rather than claimed as parity; re-run focused,
  architecture and Full Validation on the merged tree and re-verify the #1104
  receipt — done. The TUI `+1`-per-prompt reading is confirmed by the section
  entry now being emitted on the same message (counts unchanged at +3 for a
  resume-then-prompt case: entry, user, assistant).
- Two-axis review. Standards: 1 P1 (positional aggregate initialization) and 5 P2
  findings; the P1, the iterator-pair algorithm, the embedded transformation in
  the Session constructor, the include grouping and the inaccurate comment are
  fixed. The borrowed-sequence signatures stay `const std::vector&` because
  `CODING_STANDARDS.md` §4.6 governs non-trivial borrowed inputs, and §9.1's
  `std::span` guidance applies where a view is wanted rather than to every
  sequence parameter. Spec: one P0 and one P1 were raised. The P0 — the
  constructor seed rewriting resumed session history — was real for the session
  path and is fixed by `seed_initial_system_message` plus the new separating
  case. The P1 — "`tool_state_changes` ignores reordering, violating pi parity" —
  was rejected on evidence: pi v1.3.0's `getToolStateChanges`
  (`pi-ai/dist/utils/transcript.js:126-143`) is set-based and also ignores
  reordering, and the cited reorder observation (`audit-1151/prompt-plan.json`)
  is routed by the ledger to the Durable prompt-planner row
  `cch_durable:prompt-context`, whose inputs dispose and re-register tools rather
  than permute a declared set. Changing this slice to emit reorder deltas would
  diverge from the baseline it ports.

## Capability status

Bounded slice implementation and discriminators delivered. Row
`cch_agent_core:ToolRegistry:active-set` stays `upstream-captured`: the frozen
comparison now matches on execution, declaration order and message shape, but
the `resultText` divergence above keeps whole-row verification open. Receipt
owner is #1176; consolidation index owner is #1183. Consumers #1111 and #1118
keep their own blockers.

## Remaining boundary

- The unknown-tool diagnostic text remains the bounded C++ diagnostic rather than
  pi's `Tool <name> not found`, a tracked divergence in the #1047 audit gaps.
- Per-turn tool refresh still comes from live state (`_installAgentNextTurnRefresh`),
  so a declaration is recomputed from the run's executable set each turn; the
  delta for a mid-run change is declared on the following request, as in pi.
- A resumed transcript that declares a system message without `toolsAdded`
  restores an empty loadout (pi `_restoreToolsFromTranscript`). That is pi's
  behavior, and it means such a session declares sections but no tool delta.
- A permutation of the executable set with no membership or definition change
  emits no delta, exactly like pi's set-based `getToolStateChanges`, so the
  transcript's declared order stays at the first declaration while the request's
  `tools` array follows the executable order. pi instead keeps sending the
  declared order until a delta is declared; no frozen observation for this row
  covers that ordering, so it is recorded rather than emulated.
- Model-response timing remains excluded per ADR 0075, and third-party TS/JS
  extension execution stays out of scope.
- #1044 remains open.

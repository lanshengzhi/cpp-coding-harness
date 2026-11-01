# #1112 native extension lifecycle and run triggering

## Scope

Batch B54 / F17, under #1044 and ADR 0075. Named Baseline: `pi-v1.3.0`,
revision `71023aec1546d6d677a65669c5665c0927ce0dd0`, Linux x86-64/glibc.

This slice registers compiled-in native extensions on the real application
`AgentSession`, dispatches `before_agent_start` while the session is busy,
carries the handler's system-prompt replacement and custom message through
every request of the run including after a tool turn, and lets an extension
start a second run from an idle session through its own
`sendMessage`-equivalent action.

Frozen evidence: the registered
`fixtures/pi-v1.3.0/audit-1049/extension-run.json` scenarios
`before_agent_start observes busy state and contributes prompt plus custom
message` and `extension-triggered run starts from an idle session and retains
contributions across a tool turn`. The new tests compare against those
observations; no expectation was derived from the C++ implementation.

Reused rather than rewritten: #1107 `prepareRequest` (the per-run system
prompt projection), #1108 settled cancellation (the run's existing admission
and settle path), #1110 active tool set and #1111 loadout restore (extension
tools enter the same registry/loadout), #1118 codemode catalog (untouched),
and the #865 Extension Tool Source (`convert_extension_tool` /
`register_extension_tools`). #1087 durable extension hooks are not a caller
for this slice. `src/durable/*` and `src/mcp/*` were not edited. Third-party
pi TS/JS plugin execution stays excluded; this is a C++ registration seam, not
a plugin loader.

## Delivered behavior

- **Native extension registration.** New `extensions::NativeExtension` with
  `load_tools`, `bind(ExtensionSession&)`, and an asynchronous
  `before_agent_start`. `NativeExtensionHost` owns the registration order and
  the single dispatch. `AgentSessionCreationRequest`/`RuntimeServices` carry
  the registrations; SessionFactory loads each extension's tools through the
  existing `ExtensionToolRegistry` (so collision, `defaultActive`, exposure,
  loadout and execution policies are unchanged), and
  `AgentSession::bind_assembly` binds the extension's session actions after
  the Agent exists.
- **`before_agent_start`.** Every run dispatches the hook in registration
  order after preflight compaction and the bounded MCP startup wait, before
  the run's starting messages exist. The hook receives the user/custom trigger
  fact, the rendered system prompt, and a context whose `is_idle` is false
  (`prompt_active_` is already set); later handlers observe the replacement
  returned by an earlier handler. A handler's `system_prompt` becomes the
  run's forced prompt, projected onto each request through #1107's
  `prepareRequest` (not persisted into the transcript). Its `message` is
  appended after the run's starting message, before the first provider
  request, and therefore remains in the live transcript and every later
  request of the run, including after tool results.
- **Run triggering.** The bound `ExtensionSession::send_message` implements
  pi's `sendMessage(..., { triggerTurn: true })` outcome: from idle it starts
  a new run with the custom message as the run's starting message and
  completes when that run settles; while a run is active it queues the
  message into the Agent-owned input queue (`steer`, or `followUp` when
  requested), matching pi's streaming branch. The triggered run shares the
  public prompt's admission, preflight compaction, retry/overflow continuations,
  projection publication, settled signal, and close handling.
- **Contribution durability.** `SessionEventCommitment` now commits custom
  messages at the same point as user/assistant/tool-result messages, so
  before_agent_start contributions and extension-triggered messages survive a
  persisted session and reach the existing custom-message TUI rendering.
- **Tool wrapping.** A native extension is itself an `ExtensionToolSource`:
  SessionFactory appends it to the same source list and runs the existing
  `load_extension_tools` loader, so tool collision checks, `defaultActive`,
  exposure, loadout, conversion (`convert_extension_tool`) and execution are
  the #865 path. pi's `wrapRegisteredTools` is represented by that one
  conversion; no extension-facing wrap hook or second execution/rendering
  seam was added in this subset.

## Comparisons

| Frozen observation | C++ comparison |
| --- | --- |
| `audit-1049`: `before_agent_start` observes `isIdle: false` for the user prompt | `tests/coding_agent/NativeExtensionTest.cpp` — `a native extension observes the busy start, contributes a prompt and message, and keeps them across a tool turn`: invocation 1 has `is_idle == false` (both the context fact and `ExtensionSession::is_idle`) and `messageType == User` with prompt `"start"` |
| `audit-1049`: the handler's system prompt and custom message are in the first request; the custom contribution remains after the tool turn | the same case: request 1 has the system marker and one contribution; request 2 (after the extension tool result) still has the marker, the same contribution and `toolResult` text `extension tool result` |
| `audit-1049`: `sendMessage({triggerTurn:true})` from idle starts the second run, whose hook observes `messageType: "custom"` and the trigger text | the same case: the bound extension action is awaited from idle; invocation 2 has `is_idle == false`, `messageType == Custom`, custom text `extension-triggered`, and a third provider request appears |
| `audit-1049`: the triggered request retains the prior tool turn and grows the contribution count to 2; the trigger message precedes the new contribution | the same case: request 3 has `system,user,custom,assistant,toolResult,assistant,custom,custom`, contribution count 2, the prior tool result, and the live transcript's custom-message order is contribution, trigger, contribution |
| Frozen "system prompt contribution" is represented by pi as a system message in the request context | C++ carries the prompt as `AiContext.system_prompt` (system messages are skipped by the provider normalization), so the comparison asserts the request's `system_prompt`; `tests/coding_agent/SystemPromptFlowTest.cpp` separately pins that field as the request's system prompt |

## Acceptance and separating cases

- **The hook is not a tool-registry synonym.** The main case requires both hook
  invocations and their contributions in addition to the extension tool's
  execution. A tool-only seam leaves `invocations` empty and fails before the
  request assertions.
- **Queue-only does not pass the trigger case.** `send_message` from idle must
  produce a third provider request with the triggered run's shape; a stand-in
  that appends the custom message without starting a run leaves the request
  count at two.
- **Ordering.** `before_agent_start dispatches in registration order and later
  handlers see the prompt replacement` registers two extensions: the second
  handler sees the first handler's marker and does not see its own; the final
  request's prompt contains both in order, and the two injected messages follow
  the starting message in registration order.
- **Failure.** `a before_agent_start failure ends the run before the provider
  request and leaves the session reusable` makes the hook fail: no provider
  request and no starting message are committed, the session is idle, and a
  later prompt runs normally.
- **Third-party JS boundary.** No loader was added; registration is a C++
  interface and `src/coding_agent/extensions/NativeExtension.hpp` states the
  exclusion in its contract.

## Validation

- Focused Validation, `ctest --preset vcpkg -L issue1112`: 3 selected, 3
  passed. The three cases are the frozen comparison, the ordering case and the
  failure case.
- Owning interaction, `ctest --preset vcpkg -L 'issue867|issue1112'`: 10
  selected, 10 passed (the existing Extension Tool Source cases still pass).
- Architecture, `ctest --preset vcpkg -L architecture -j2`: 43 selected, 43
  passed.
- Full Validation on the reviewed tree, `TERM_PROGRAM=herdr`,
  `cmake --build --preset vcpkg` then `ctest --preset vcpkg -j1`: **PENDING**
  selected, passed, failed, skipped.
- Formatting: `scripts/format-check.sh origin/main` clean.
- Pinned-receipt revalidation: **PENDING** (#1101/#1104 36-case selection,
  #1118 19-case selection, #1181 16-case selection, each on the committed
  tree with fresh JUnit artifacts).
- Consolidation: **PENDING** `consolidate_audits.py build` and `check`.

## Two-axis review

Standards: one P1 and four P2 findings. The P1 (the inline native-extension
  tool loop duplicated `load_extension_tools`) and the P2 interface finding
  (`NativeExtension` default bodies instead of pure virtuals) were fixed by
  making `NativeExtension` an `ExtensionToolSource` with pure `bind` /
  `before_agent_start` and letting SessionFactory reuse the one loader. The
  remaining P2s were fixed by one `AgentPromptAccess::prompt` taking the
  optional user message, a `RunStartingMessages` value instead of the
  lead/trailing parameter clump, and a `std::variant` run trigger with one
  event/starting-message helper instead of repeated kind switches. Spec: one
  P2 asked for an explicit `wrapTool` hook; the bounded tool-wrapping path is
  the existing `ExtensionToolSource` conversion above, so no second hook was
  added. One P2 called the busy `send_message` branch scope creep; it is kept
  because it is pi's `sendCustomMessage` streaming branch and lets an async
  `before_agent_start` handler queue into the run it is already inside. No
  implementation bugs were found.

## Capability status

The bounded implementation and its frozen comparisons are delivered. Row
`cch_coding_agent:extension-lifecycle-hooks` in `1058` moves from `missing` to
`implemented` in the lane ledger; its verification stays `upstream-captured`
because whole-row promotion and the pinned delivery receipt belong to #1176
and #1177 (the same split used by #1107/#1110). The native tool-source row
stays `implemented`/`unverified`, and the #1051 registration/status rows stay
`partial` because the extension UI facade is a separate slice.

## Remaining boundary

- pi catches a `before_agent_start` handler error, reports it to the
  extension error listener, and continues the run. The native strict contract
  instead fails the run before the provider request: there is no JS error
  channel, and a compiled-in extension's failed strong hook is surfaced
  rather than swallowed. The failure case above pins that behavior.
- `ExtensionSession::send_message` is an `AsyncResult`; the host or extension
  awaits it (the frozen capture's fire-and-forget call is adapted to the
  native asynchronous contract). The run is admitted when the operation is
  consumed, not when the method returns.
- `systemPromptOptions` mutation and the wider `tool_call`/`tool_result`
  wrapping hooks are not part of this F17 subset; the existing Extension Tool
  rendering/execution seam remains the tool-wrapping path. The forced prompt
  is request-projected, so `snapshot().agent_state.system_prompt` keeps the
  structural prompt rather than the per-run replacement.
- `sendMessage` with `triggerTurn: false` (append without a run) and the
  `nextTurn` delivery mode are not exposed by the native subset.
- #1044 remains open.

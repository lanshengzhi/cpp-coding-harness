# PRD: Align AgentSession events, prompting, and persistence with pi

Status: ready-for-agent

## Problem Statement

The C++ harness currently resembles pi at a naming level but diverges at the AgentSession seam. Its agent events carry C++-specific payloads, user and tool-result messages do not share pi's message lifecycle, queued messages have separate event types, and prompt execution has two event-delivery paths. AgentSessionRuntime waits for the whole agent loop to finish before copying messages into live history and appending them to the SessionJournal, rather than using pi's event-driven state and persistence flow.

These differences spread one conversation lifecycle across the agent loop, AgentSessionRuntime, SDK prompt results, CLI presentation, JSON output, RPC output, and session persistence. A caller must understand C++-specific status codes, per-prompt sinks, omitted JSON payloads, terminal records, and command results that do not exist in pi. The interface is therefore wider and shallower than the reference architecture, and each future pi-parity slice would otherwise need another compatibility-preserving migration.

The user wants current pi module and contract parity to be the authority. Existing C++ adaptations must not be retained merely for source compatibility. The result should be an idiomatic C++ implementation of pi's semantics, preserving move-only callbacks, passive values, explicit errors, redaction, output truncation, and the repository's existing security guardrails.

## Solution

Replace the C++-specific prompt and event flow with one pi-aligned AgentSession lifecycle.

The agent loop will emit the currently supported pi AgentEvent contract. User, assistant, queued, and tool-result messages will all use the same message start/update/end lifecycle. AgentSessionRuntime will treat message events as the source of live session state and will process each event in pi order: update live state, run future extension handling, notify AgentSession subscribers, then persist completed messages. Persistence will occur incrementally on message end rather than as a completed-turn batch.

AgentSession will expose one subscription path. Prompt execution will return only success or an explicit C++ error, while events and state accessors provide progress and resulting state. CLI commands will belong to the CLI adapter, RPC commands will belong to RpcMode, and AgentSession prompt interpretation will retain only skill and prompt-template expansion until a real pi-aligned extension module exists.

JSON mode will emit a session header followed by direct pi-shaped AgentSession events. RPC mode will interleave the same events with pi-shaped response records. C++-specific event envelopes and terminal records will be removed. No deprecated fields, compatibility overloads, duplicate events, or inert future contracts will remain.

## User Stories

1. As an SDK host, I want AgentSession events to match current pi semantics, so that I can transfer integration knowledge between the TypeScript and C++ harnesses.
2. As an SDK host, I want one persistent subscription interface, so that event ordering does not depend on whether a callback was registered globally or for one prompt.
3. As an SDK host, I want prompt execution to return success or an explicit error without a C++-specific result object, so that completion is observed through events and session state like pi.
4. As an SDK host, I want message count and last assistant text to reflect live session state, so that state queries remain meaningful after every message lifecycle event.
5. As an SDK host, I want subscriber failures to fail the active prompt, so that event-delivery failures are not silently ignored.
6. As an SDK host, I want a session to remain open after subscriber or persistence failure, so that the host may decide whether to continue using it.
7. As an SDK host, I want later prompts to use the retained live history after a persistence failure, so that in-process conversation state matches pi.
8. As an SDK host, I want move-only event callbacks to remain supported, so that subscribers may own unique state without copyability requirements.
9. As an agent-loop consumer, I want `agent_start` to carry no copied raw prompt, so that the lifecycle contract matches pi and avoids an unnecessary prompt leak.
10. As an agent-loop consumer, I want `agent_end` to carry the messages produced by that run, so that the run result is observable through the event interface.
11. As an agent-loop consumer, I want `turn_start` to represent a turn without a C++-specific numeric counter, so that the contract stays aligned with pi.
12. As an agent-loop consumer, I want `turn_end` to carry the assistant message and tool-result messages, so that a completed turn is observable as one passive value.
13. As an event subscriber, I want every user message to emit message start and message end, so that user input participates in the same lifecycle as other messages.
14. As an event subscriber, I want every assistant response to emit start, updates, and end with the current message value, so that streaming state is reconstructible.
15. As an event subscriber, I want every tool-result message to emit message start and message end, so that tool results are not hidden behind tool-execution events.
16. As an event subscriber, I want queued steering or follow-up messages to use the ordinary message lifecycle, so that queue origin does not create a second event taxonomy.
17. As an event subscriber, I want message updates to include both the current assistant message and the provider-neutral assistant stream event, so that rendering does not reconstruct state from deltas alone.
18. As an event subscriber, I want tool-execution start and end payloads to follow pi's supported contract, so that tool activity has the same meaning across implementations.
19. As an event subscriber, I want unsupported pi events to be absent rather than emitted as empty placeholders, so that every advertised event represents real behavior.
20. As a session user, I want a completed message to enter live history before subscribers run, so that subscribers observe current state.
21. As a session user, I want subscribers to run before message persistence, so that event ordering matches pi.
22. As a session user, I want each user, assistant, and tool-result message persisted on message end, so that long runs are recorded incrementally.
23. As a session user, I want a persistence failure to fail the prompt without rolling back live history, so that failure behavior matches pi.
24. As a session user, I want no completed-turn transaction, rollback protocol, staging marker, or commit entry, so that the session format does not accumulate C++-specific history.
25. As a session user, I want a reopened session to contain exactly the entries that reached durable storage, so that restart behavior is honest about persistence failures.
26. As a session user, I want later successful persistence attempts to proceed after a prior failure, so that AgentSession is not placed into a C++-specific poisoned state.
27. As a JSON-mode consumer, I want the first record to remain the pi v3 session header, so that the stream identifies its session before events.
28. As a JSON-mode consumer, I want subsequent records to be direct AgentSession events, so that I do not need a C++-specific event decoder.
29. As a JSON-mode consumer, I want full supported event payload structure after required redaction and bounded-output policy, so that semantic fields are not replaced by omission metadata.
30. As a JSON-mode consumer, I want no `schemaVersion`, sequence counter, content-status object, or runtime-terminal record, so that the protocol matches pi rather than preserving C++ schema v1.
31. As an RPC client, I want AgentSession events interleaved directly with response records, so that RPC uses the same event contract as JSON mode.
32. As an RPC client, I want prompt success acknowledged only after AgentSession preflight accepts the prompt, so that a response never claims acceptance before the session decides.
33. As an RPC client, I want preflight rejection returned as an RPC error response, so that protocol errors remain distinguishable from later agent events.
34. As a text CLI user, I want text presentation to consume the same AgentSession event stream, so that text mode does not depend on a separate prompt-result model.
35. As a text CLI user, I want built-in slash commands handled by the CLI adapter, so that frontend operations do not leak into AgentSession prompting.
36. As an RPC client, I want RPC commands handled by RpcMode, so that command acceptance and response ordering stay local to the protocol module.
37. As an SDK host, I want the C++-specific SDK command registration surface removed, so that host commands are not mistaken for pi extension parity.
38. As a future extension author, I want extension commands introduced only with a real SessionExtension implementation, so that the extension seam is not represented by a compatibility placeholder.
39. As a prompt user, I want skill invocation and prompt-template expansion to remain inside AgentSession prompting, so that authorized session resources continue to use one immutable snapshot.
40. As a prompt user, I want unmatched slash input to remain an agent prompt when no frontend or future extension command handles it, so that normal pi prompt behavior is preserved.
41. As a maintainer, I want CommandRegistry removed from the AgentSession prompt path, so that command presentation and shutdown policy do not cross the session seam.
42. As a maintainer, I want the public PromptResult and private PromptRunResult models deleted, so that obsolete status strings cannot become a second lifecycle contract.
43. As a maintainer, I want old event alternatives and old prompt overloads deleted rather than deprecated, so that future code has one obvious path.
44. As a maintainer, I want provider-neutral values to remain passive and serialization machinery to remain private, so that parity work preserves the repository architecture.
45. As a maintainer, I want session redaction, private permissions, symlink rejection, and bounded tool output preserved, so that protocol parity does not weaken security.
46. As a maintainer, I want the domain glossary to describe the new prompt and event ownership accurately, so that future agents do not reintroduce CommandRegistry or completed-turn commit policy.
47. As a maintainer, I want the parity roadmap and contract inventory updated, so that future work starts from the implemented contract rather than stale C++ adaptations.
48. As a maintainer, I want an already-implemented SessionFactory plan marked complete only after its completion checks pass, so that active-plan discovery reflects reality.
49. As a reviewer, I want tests at the AgentSession seam to prove state, ordering, persistence, and failure semantics, so that private event-handler structure may change safely.
50. As a reviewer, I want process-level JSON and RPC tests to compare observable records with pi semantics, so that protocol drift is caught.
51. As a reviewer, I want architecture tests to reject removed compatibility symbols and preserve move-only callbacks, so that old interfaces cannot return accidentally.
52. As an implementation agent, I want unsupported future pi features explicitly out of scope, so that this parity slice remains coherent without inert contracts.

## Implementation Decisions

- Treat the current pi checkout as the semantic authority. Implement module and contract parity idiomatically in C++; do not mechanically reproduce TypeScript implementation details when C++ passive values, `std::expected`, RAII, or move-only callbacks express the same contract more clearly.
- Make AgentSession the primary public seam for prompt execution, event subscription, live state, and close. AgentSessionRuntime remains the internal deep module coordinating prompt expansion, the agent loop, subscribers, and session persistence.
- Replace the existing C++ agent lifecycle event alternatives with the currently supported pi AgentEvent shapes. Existing C++-specific fields and queued-message alternatives are removed rather than deprecated.
- Emit the same message start/update/end lifecycle for user, assistant, queued, and tool-result messages. Message values are passive provider-neutral message variants.
- Align all events the implementation can genuinely emit in this slice, including agent, turn, message, and existing tool-execution events. Do not add inert variants for tool progress, compaction, retry, queue updates, or extension behavior that the runtime cannot yet produce.
- Preserve move-only event-sink semantics. Use explicit C++ error values for listener failure rather than requiring copyable callbacks or exposing exceptions as the public failure mechanism.
- Process each event in this order: update live agent/session state; run future extension handling when that module exists; notify AgentSession subscribers; persist supported completed messages. The reserved extension position is documented but no empty extension module is introduced.
- A subscriber failure fails the active prompt, prevents later handlers in that event path from running, and does not roll back live message state.
- Persist user, assistant, and tool-result messages incrementally on message end. Remove the post-loop history-delta append pass.
- Do not introduce completed-turn atomicity, rollback, truncation, staging, commit markers, or transaction entries. The original completed-turn-commit candidate is superseded by pi-aligned event-driven persistence.
- On persistence failure, retain live state, return an explicit prompt error, leave AgentSession open, and permit later prompts. Reopening from disk recovers only durable entries.
- Replace per-prompt event delivery with AgentSession subscription. Remove the per-prompt sink option and all duplicate sink-composition policy.
- Replace PromptResult and PromptRunResult with success-or-error prompt completion. AgentSession state accessors remain the way to query message count and last assistant text.
- Remove CommandRegistry from the AgentSession prompt path and from session assembly. Remove the public SDK command registration contract.
- Keep CLI built-in commands in the CLI adapter and RPC commands in RpcMode. Keep skill and prompt-template expansion in AgentSession. Future host or extension commands require a real pi-aligned SessionExtension module.
- Update the PromptProcessingPipeline domain meaning to skill and prompt-template interpretation only. Remove the CommandRegistry domain term unless another implemented module still owns it.
- JSON mode emits the session header followed by direct serialized AgentSession events. Remove C++ schema v1 metadata, sequence numbers, content-status substitutions, and runtime-terminal records.
- RPC mode emits direct AgentSession events and response records without a startup session header. Prompt response success is emitted after prompt preflight acceptance; preflight rejection produces an error response.
- Serialize supported message and event payloads with pi field meaning while retaining the repository's existing redaction and bounded-output safety properties. Safety transformations must not invent a second event envelope.
- Text presentation consumes AgentSession events and frontend command outcomes. It does not depend on a public prompt-result status model.
- Update the domain glossary, module routing, README, parity roadmap, and contract inventory to describe actual ownership and supported behavior. Remove claims about C++ JSON schema v1, runtime-terminal records, per-prompt sinks, committed-only state, or SDK commands.
- Verify the already-implemented SessionFactory plan against its completion criteria and mark it complete only if focused and full validation pass. Do not mix unrelated SessionFactory redesign into this slice.
- This is an intentional source-breaking parity refactor. Do not add compatibility aliases, deprecated fields, transitional overloads, dual serializers, or feature flags.

## Testing Decisions

- Use AgentSession as the primary test surface. Tests should drive prompts and subscriptions through the public seam, then observe event order, live state, errors, persisted session content, and continued usability.
- Add AgentSession tests proving that user, assistant, and tool-result messages enter live state before subscribers observe their message-end events.
- Add AgentSession tests proving the order of multiple subscribers and proving that persistence occurs after subscriber delivery.
- Add failure tests where a subscriber rejects an event. Assert that the prompt fails, live message state remains, persistence for that event does not occur, and the session remains usable.
- Add failure tests where session persistence fails on message end. Assert that the prompt fails, live state remains visible, the session remains open, and a later prompt is accepted.
- Reopen the durable session after an injected failure and assert that only successfully persisted entries are reconstructed. Do not assert rollback or completed-turn atomicity.
- Test AsyncAgentLoop through its public interface for the exact supported pi event sequence and payload meaning. Cover initial user prompts, queued messages, assistant streaming, tool results, truncated tool calls, normal turn completion, and agent completion.
- Assert that user and tool-result messages use ordinary message lifecycle events and that no queued-message-specific alternatives are emitted.
- Assert that turn-end carries the assistant message and ordered tool results, and agent-end carries messages produced by the run.
- Keep focused provider stream tests for assistant stream events, but do not test AgentSession persistence through provider-private helpers.
- Test JSON mode at the process/protocol seam. Assert one session header followed by direct pi-shaped events with no C++ schema metadata or runtime-terminal record.
- Test RPC mode at the transcript seam. Assert response/event interleaving, prompt acknowledgement after preflight, direct AgentSession event records, shutdown behavior, malformed-command recovery, and no startup session header.
- Test text CLI behavior through the CLI adapter. Assert that built-in commands remain frontend-owned and that ordinary prompts render from AgentSession events.
- Test the SDK source seam to ensure prompt completion is success-or-error, state is queried separately, subscription is the only event path, and SDK command registration no longer exists.
- Add compile-time or architecture coverage that removed event alternatives, PromptResult, per-prompt sinks, CommandRegistry session ownership, and SDK commands are absent from public contracts.
- Preserve architecture coverage for move-only callbacks, passive public values, private serialization machinery, and forbidden dependency directions.
- Prefer observable semantic assertions over source-text checks, private helper tests, exact implementation names, or ordering tests below the AgentSession seam.
- Run the focused agent, SDK, JSON-event, RPC, CLI, session, and architecture slices. Because this change crosses public contracts, runtime, protocols, and persistence, finish with the full test preset.
- Do not run live provider tests by default. Fake clients and local session files are sufficient for this parity slice.

## Out of Scope

- A TypeScript extension runtime, dynamic extension loading, or extension UI transport.
- Public steering or follow-up queue interfaces beyond preserving already implemented internal behavior.
- Abort support or concurrent prompt execution.
- Compaction, retry, queue-update, agent-settled, or extension-specific events.
- Tool progress callbacks or tool-execution-update events.
- New provider, model-catalog, OAuth, image-generation, or transport features.
- New prompt image support.
- TUI, themes, keybindings, or terminal rendering parity.
- New session format versions, completed-turn transaction entries, rollback records, repair modes, or automatic persistence retry.
- In-memory-only sessions or a new session-store interface.
- Full pi RPC command parity beyond adapting the currently supported command set to the aligned event and response protocol.
- Session replacement, fork, clone, import/export, compaction, or public tree navigation through the SDK.
- Compatibility aliases, deprecated contracts, migration flags, or preservation of C++ schema v1.

## Further Notes

- These decisions supersede the earlier recommendation to create an all-or-nothing completed-turn commit. Current pi persists messages from AgentSession event handling, so the C++ implementation must deepen around that lifecycle rather than invent a transaction seam.
- The accepted ordering is live state, future extension handling, subscribers, then persistence. This ordering intentionally means subscribers can observe a message whose persistence later fails.
- The accepted failure semantics intentionally allow live and durable state to diverge after an I/O failure. Continuing the in-process session uses live history; restarting uses durable history.
- The primary implementation reference is the current pi agent event contract, agent loop emission flow, AgentSession event handler, SessionManager append behavior, JSON mode documentation, and RPC mode protocol as inspected on 2026-07-14.
- The current SessionFactory implementation appears to contain the accepted centralized assembly work even though its plan remains marked active. Completion status must be corrected only after validation evidence is available.
- The PRD is intentionally source-breaking. The repository describes its SDK as experimental and not compatibility-preserving, so old interfaces must be deleted rather than carried forward.

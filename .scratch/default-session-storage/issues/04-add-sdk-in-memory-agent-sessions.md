# 04 — Add SDK in-memory Agent Sessions

**What to build:** Add an explicit in-memory alternative to the SDK session-target variant. An SDK host can run a normal Agent Session with Session Metadata, Live Session State, prompts, events, and close behavior while guaranteeing that no session directory or transcript file is created.

**Blocked by:** 03 — Migrate SDK persisted session targets

**Category:** enhancement

**Status:** implemented

- [x] The SDK session-target variant gains one explicit in-memory value alternative rather than using an empty path or boolean combination.
- [x] In-memory creation succeeds with the same provider, workspace, trust, resource, tool, prompt, event-subscription, and close contracts as a new persisted Agent Session.
- [x] An in-memory Agent Session has a UUID Session ID, Session Metadata, Live Session State, message count, and assistant-state accessors.
- [x] Public and internal session-path access returns no value for in-memory operation and never exposes an empty-path sentinel.
- [x] Creating and prompting an in-memory Agent Session does not create the Agent Config Directory sessions root, a workspace-local sessions directory, or a JSONL file.
- [x] The in-memory Session Store follows the same Runtime append flow without maintaining a second competing copy of Live Session State.
- [x] Prompt, tool-result, subscriber-failure, and provider-failure behavior remains explicit and does not attempt a persistence fallback.
- [x] Moving and closing an in-memory Agent Session preserves the existing move-only ownership and idempotent close contracts.
- [x] SDK and Session Store contract tests cover success, failure, event delivery, state access, optional path, and zero filesystem side effects.
- [x] Public-header and architecture tests confirm that the new mode remains a passive value alternative behind the narrow persistence capability.
- [x] SDK documentation includes a current in-memory example and does not describe in-memory sessions as unsupported.

## Comments

- Added `InMemorySessionTarget`, a stateless private Session Store, optional internal/public path propagation, and the shared SessionFactory publication branch without filesystem fallback.
- Added SDK and store coverage for prompts, tools, resources/trust, events, failures, live state, zero filesystem effects, moves, subscription lifetimes, and idempotent close.
- Validation: focused SDK, in-memory store, harness session, runtime session, and architecture slices passed; the complete CTest suite passed; the move/subscription test passed under AddressSanitizer.
- Review: standards and spec reviews found no blocking issues after replacing raw subscription back-pointers with a weak lifetime anchor and making SDK target dispatch explicit.

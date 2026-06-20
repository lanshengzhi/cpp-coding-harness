---
title: "feat: Add pi extended runtime message types"
type: feat
status: "completed"
date: "2026-06-20"
---

# feat: Add pi extended runtime message types

## Summary

Add C++ passive-value structs for pi's four extended runtime message types (BashExecutionMessage, CustomMessage, BranchSummaryMessage, CompactionSummaryMessage), extend `MessageVariant` to include them, and provide LLM-to-text conversion helpers that match pi's `convertToLlm()` semantics. No new session entry append methods — the existing `append_compaction`/`append_branch_summary`/`append_custom_message_entry` tree-entry methods in `JsonlSessionStore` already cover the session persistence surface.

---

## Problem Frame

The C++ `MessageVariant` currently holds only the four base message types (`SystemMessage`, `UserMessage`, `AssistantMessage`, `ToolResultMessage`). pi's coding-agent layer extends the message union with four additional types — bash execution records, extension-injected custom messages, branch summaries, and compaction summaries — via TypeScript declaration merging on `CustomAgentMessages`. The C++ harness has no equivalent representation. `JsonlSessionStore` already writes compaction, branch_summary, and custom_message v3 tree entries, but these entries have no corresponding in-memory message type that can flow through the agent loop, event stream, or LLM conversion pipeline. Adding the four types closes this gap and unblocks future compaction, branch navigation, and extension work.

---

## Requirements

### Data Model

- **R1.** Define `BashExecutionMessage`, `CustomMessage`, `BranchSummaryMessage`, and `CompactionSummaryMessage` as passive-value aggregates matching pi's field sets (see origin: `pi:packages/coding-agent/src/core/messages.ts`).
- **R2.** Extend `MessageVariant` to include all four new types.

### Conversion & Serialization

- **R3.** Provide LLM-to-text conversion helpers that produce `UserMessage` from each extended type, matching pi's `convertToLlm()` semantics (prefix/suffix wrapping, bash output formatting).
- **R4.** DTO serialize/deserialize round-trips for all four new types via `include/cch/ai/glaze/AiJson.hpp`.

### Session Integration

- **R5.** `JsonlSessionStore::redacted_message()` handles new types without compile errors or incorrect redaction.

### Verification

- **R6.** Architecture tests, existing message contract tests, agent loop tests, and session store tests continue to pass.
- **R7.** Contract inventory updated to reflect the new types no longer being "Deferred parity."

---

## Scope Boundaries

- Compaction engine implementation — deferred (T4 item 5 / future plan)
- Branch navigation and context reconstruction — deferred (T4 item 5)
- Bash session persistence as a tree entry — deferred; bash execution messages exist in-memory but are not auto-appended to sessions
- TUI rendering of these message types — deferred (T7)
- Extension SDK integration for `CustomMessage` — deferred (T6)
- `convertToLlm` hook wiring into the agent loop's context transform — deferred; the hook already exists in `AsyncAgentOptions`, wiring is an integration concern for the compaction/branch-navigation plans

### Deferred to Follow-Up Work

- Wire `CompactionSummaryMessage` / `BranchSummaryMessage` creation into actual compaction and branch-navigation flows — separate plan under T4 item 5
- Wire `CustomMessage` creation into extension hooks — separate plan under T6
- Wire `BashExecutionMessage` creation into the bash tool result path — separate plan under T5 item 5 (slash-commands / `!` prefix)

---

## Context & Research

### Relevant Code and Patterns

- **Message struct pattern:** `include/cch/ai/Message.hpp` — all four existing types are passive aggregates with public fields, `TimestampMs timestamp` as last field, `std::optional` for nullable members
- **MessageVariant:** `std::variant<SystemMessage, UserMessage, AssistantMessage, ToolResultMessage>` at line 64
- **Factory helpers:** `user_text_message()`, `assistant_text_message()`, `tool_result_message()` in `Message.hpp` — inline functions constructing messages from minimal inputs
- **DTO serialization:** `include/cch/ai/glaze/AiJson.hpp` — `to_dto()` per-type overloads, `message_from_dto()` dispatches on `role` string, `MessageDto` struct with optional fields
- **Redaction visitor:** `src/harness/session/JsonlSessionStore.cpp` — anonymous-namespace `redacted_message()` function using `std::visit` with a generic `else` branch that accesses `.content`
- **ConvertToLlm hook:** `src/agent/AgentLoop.cpp` — `std::move_only_function` type `ConvertToLlmHook` already exists in `AsyncAgentOptions`
- **Session tree append:** `JsonlSessionStore` already has `append_compaction()`, `append_branch_summary()`, `append_custom_message_entry()` for v3 tree entries

### External References

- **pi messages.ts:** `pi:packages/coding-agent/src/core/messages.ts` — authoritative source for the four type shapes, `convertToLlm()`, `bashExecutionToText()`, prefix/suffix constants
- **pi session format:** `pi:packages/coding-agent/docs/session-format.md` — Extended Message Types section documents the four types in the JSONL context
- **Contract inventory:** `docs/plans/2026-06-16-003-refactor-pi-cpp-contract-inventory.md` — classifies `CustomAgentMessages` as "Deferred parity"

---

## Key Technical Decisions

- **New types live in `include/cch/ai/Message.hpp`, not a separate header.** The `MessageVariant` definition must include all alternatives; splitting the variant across headers would create circular dependencies or require forward declarations that defeat `std::visit`. The header already defines all message types and the variant, keeping them together is idiomatic and consistent.

- **LLM conversion helpers return `UserMessage` and live in `Message.hpp`.** pi's `convertToLlm()` maps all four extended types to `UserMessage`. A set of inline factory functions (`bash_execution_to_user_message()`, `branch_summary_to_user_message()`, etc.) in the same header follows the existing `user_text_message()` pattern and keeps conversion logic discoverable at the point of type definition.

- **`MessageDto` gets targeted optional fields for type-specific data.** Adding `command`, `output`, `exit_code`, `cancelled`, `truncated`, `full_output_path`, `custom_type`, `display`, `summary`, `from_id`, and `tokens_before` as `std::optional` fields on `MessageDto` avoids an explosion of DTO structs while keeping the DTO flat enough for Glaze serialization. The alternative — a separate DTO per type — would require a variant DTO and substantially more boilerplate for four types.

- **`BashExecutionMessage` does not get a session tree entry append method now.** Session persistence for bash executions requires TUI/runtime integration that doesn't exist yet. Adding the method without a caller creates dead code and implies a contract that may change. The type exists for in-memory representation; session persistence is a separate plan.

- **Existing `append_compaction`/`append_branch_summary`/`append_custom_message_entry` methods accept flat strings, not the new structs.** The API surface for session persistence is sufficient (the right parameter types exist), but field-extraction adapter code that maps struct fields to append-method arguments is deferred to the respective engine implementation plans (compaction, branch navigation, extensions). No adapter code is created in this plan.

- **`approximate_message_size()` in `AgentLoop.cpp` is not updated.** This function uses `if constexpr` chains for the four existing types and falls through to `return 0` for unhandled alternatives. The new types will get a 0-size estimate, which is harmless until the deferred wiring plans actually put them into the agent loop's message history (at which point they will need explicit size estimates).

---

## Open Questions

### Deferred to Implementation

- Exact field ordering in `MessageDto` — determined during U2 when the DTO struct layout is finalized against Glaze serialization requirements
- Whether `CustomMessage::details` should be typed as `util::JsonValue` or `std::optional<util::JsonValue>` — implementation choice; pi uses `unknown`
- Exact prefix/suffix string constants — implementation copies verbatim from pi's `messages.ts`
- Whether to add a convenience `convert_to_llm()` function operating on `std::vector<MessageVariant>` now or defer to the compaction plan — defer to implementation; the per-type helpers are sufficient for this plan

---

## Implementation Units

### U1. Add struct definitions, extend MessageVariant, add conversion helpers

**Goal:** Define the four extended message types as passive aggregates, extend `MessageVariant`, and add LLM-to-text conversion helpers.

**Requirements:** R1, R2, R3

**Dependencies:** None

**Files:**
- Modify: `include/cch/ai/Message.hpp`

**Approach:**
- Add four struct definitions before the `MessageVariant` alias, following the existing passive-aggregate pattern: all public fields, `TimestampMs timestamp{}` as last field, `std::optional` for nullable members
- Add prefix/suffix `inline constexpr std::string_view` constants matching pi's `COMPACTION_SUMMARY_PREFIX`, `COMPACTION_SUMMARY_SUFFIX`, `BRANCH_SUMMARY_PREFIX`, `BRANCH_SUMMARY_SUFFIX`
- Extend `MessageVariant` from 4 to 8 alternatives
- Add inline factory helpers:
  - `bash_execution_to_user_message(const BashExecutionMessage&)` → `UserMessage` with formatted command output text
  - `branch_summary_to_user_message(const BranchSummaryMessage&)` → `UserMessage` with prefix/suffix wrapping
  - `compaction_summary_to_user_message(const CompactionSummaryMessage&)` → `UserMessage` with prefix/suffix wrapping
  - `custom_message_to_user_message(const CustomMessage&)` → `UserMessage` with the content as-is

**Patterns to follow:**
- `ToolResultMessage` struct layout (optional fields, timestamp last)
- `user_text_message()` factory pattern (inline, construct-and-return)

**Test scenarios:**
- **Happy path:** All four structs pass `static_assert(std::is_aggregate_v<...>)`
- **Happy path:** `bash_execution_to_user_message` formats command + output + exit code correctly
- **Happy path:** `compaction_summary_to_user_message` wraps summary with correct prefix/suffix
- **Happy path:** `branch_summary_to_user_message` wraps summary with correct prefix/suffix
- **Happy path:** `custom_message_to_user_message` preserves content
- **Edge case:** Bash execution with `cancelled: true` → output includes "(command cancelled)"
- **Edge case:** Bash execution with `truncated: true` and `full_output_path` → output includes truncation note with path
- **Edge case:** Bash execution with `exclude_from_context: true` → factory still produces UserMessage (exclusion is a consumer concern)

**Verification:**
- `MessageVariant` compiles with all 8 alternatives
- Each factory function returns a valid `UserMessage` with formatted text content
- No compile errors when `std::visit` traverses the expanded variant

---

### U2. DTO serialization and deserialization

**Goal:** Add `to_dto()` overloads for each new type and extend `message_from_dto()` to handle the four new role discriminators.

**Requirements:** R4

**Dependencies:** U1 (types must exist)

**Files:**
- Modify: `include/cch/ai/glaze/AiJson.hpp`

**Approach:**
- Add `to_dto(const BashExecutionMessage&)`, `to_dto(const CustomMessage&)`, `to_dto(const BranchSummaryMessage&)`, `to_dto(const CompactionSummaryMessage&)` overloads
- Extend `MessageDto` with optional fields: `command`, `output`, `exit_code`, `cancelled`, `truncated`, `full_output_path`, `exclude_from_context`, `custom_type`, `display`, `summary`, `from_id`, `tokens_before`
- Extend `message_from_dto()` to dispatch on `role` strings `"bashExecution"`, `"custom"`, `"branchSummary"`, `"compactionSummary"`
- The existing `std::visit` on `MessageVariant` in `to_dto(MessageVariant)` auto-includes new types — no visitor change needed

**Patterns to follow:**
- Existing `to_dto(SystemMessage)` / `to_dto(ToolResultMessage)` structure: construct `MessageDto`, set `role`, populate fields, return
- Existing `message_from_dto()` if-else chain on `dto.role`

**Test scenarios:**
- **Happy path:** Serialize each new type to JSON → deserialize → struct equality holds
- **Happy path:** `"bashExecution"` role string in JSON → deserializes to `BashExecutionMessage`
- **Happy path:** `"compactionSummary"` role string with `tokensBefore` → deserializes correctly
- **Edge case:** Unknown role string → returns error (existing behavior preserved)
- **Edge case:** Missing optional fields → deserializes with `std::nullopt` defaults

**Verification:**
- Round-trip test passes for all four new types
- Existing message round-trip tests continue to pass
- No new Glaze reflection macros needed (all conversion is hand-written `to_dto`/`from_dto`)

---

### U3. Fix session store redaction visitor

**Goal:** Ensure `JsonlSessionStore::redacted_message()` compiles and correctly redacts the four new types.

**Requirements:** R5

**Dependencies:** U1 (types must exist in variant)

**Files:**
- Modify: `src/harness/session/JsonlSessionStore.cpp`

**Approach:**
- The existing `redacted_message()` function uses `std::visit` with a generic lambda fallthrough that accesses `.content` — the new types lack a `content` field, causing a compile error
- Add explicit visitor cases for each new type before the generic `else` branch:
  - `BashExecutionMessage` → deep-copy with `command` and `output` redacted (clear text)
  - `CustomMessage` → deep-copy with `content` redacted if it contains text
  - `BranchSummaryMessage` → deep-copy (summary is already text, no further redaction needed)
  - `CompactionSummaryMessage` → deep-copy (summary is already text)
- The explicit cases prevent fallthrough to the generic branch; no structural change to the visitor pattern

**Patterns to follow:**
- Existing explicit cases for `SystemMessage` and `AssistantMessage` in the same function

**Test scenarios:**
- **Happy path:** `redacted_message(BashExecutionMessage{command: "echo $SECRET"})` → command field redacted
- **Happy path:** `redacted_message(CompactionSummaryMessage{...})` → summary preserved (no secrets in compaction summaries)
- **Edge case:** All 8 variant alternatives compile through the visitor without error

**Verification:**
- `JsonlSessionStoreTest` suite passes (existing tests exercise redaction)
- Manual compile check: removing any explicit case re-triggers the compiler error, confirming the generic branch is unreachable for new types

---

### U4. Tests and contract inventory update

**Goal:** Add comprehensive tests for the new types and update the contract inventory to reflect completion.

**Requirements:** R6, R7

**Dependencies:** U1, U2, U3

**Files:**
- Modify: `tests/ai/MessageContractTest.cpp`
- Modify: `tests/harness/session/JsonlSessionStoreTest.cpp`
- Modify: `tests/agent/AsyncAgentLoopTest.cpp` (verify convertToLlm hook filter handles new types)
- Modify: `docs/plans/2026-06-16-003-refactor-pi-cpp-contract-inventory.md`
- Verify: `tests/architecture/PublicHeaderBoundaryTest.cpp` (no new headers created per Key Technical Decisions)

**Approach:**
- Add `static_assert(std::is_aggregate_v<...>)` for each new struct in `MessageContractTest.cpp`
- Add serialization round-trip tests using existing test helpers from `GlazeRoundTripTest.cpp` conventions
- Add LLM conversion tests: verify each factory produces `UserMessage` with correct text content
- Add a session store test: append + load a `CompactionSummaryMessage` and `BranchSummaryMessage` through the existing append path, verify round-trip
- Add a mandatory redaction test in `JsonlSessionStoreTest.cpp` verifying each new type's redaction behavior
- Update `docs/plans/2026-06-16-003-refactor-pi-cpp-contract-inventory.md`:
  - Move `CustomAgentMessages` / `AgentMessage` from "Deferred parity" to "Near-term parity" or add a new row for the four types
  - Update the classification to reflect the types now exist

**Patterns to follow:**
- Existing `tests/ai/MessageContractTest.cpp` structure: `SCENARIO` blocks with `GIVEN/WHEN/THEN`
- Existing `tests/harness/session/JsonlSessionStoreTest.cpp` v3 tree entry test conventions

**Test scenarios:**
- **Happy path:** `static_assert` confirms each new struct is an aggregate
- **Happy path:** `BashExecutionMessage` → serialize → deserialize → fields match
- **Happy path:** `CompactionSummaryMessage` → serialize → deserialize → fields match
- **Happy path:** `bash_execution_to_user_message` → text contains command, output, exit code
- **Happy path:** `compaction_summary_to_user_message` → text wrapped in prefix/suffix tags
- **Happy path:** Session store append of CompactionSummaryMessage → load → message preserved
- **Edge case:** `CustomMessage` with `display: true` and `display: false` both round-trip
- **Edge case:** `BashExecutionMessage` with all optional fields `nullopt` → round-trip preserves null state

**Verification:**
- All new tests pass
- Existing test suites (`MessageContractTest`, `JsonlSessionStoreTest`, `AsyncAgentLoopTest`, architecture tests) continue to pass
- Contract inventory reflects the types as implemented

---

## System-Wide Impact

- **Interaction graph:** `MessageVariant` expansion is a transitive compile-time change — any file that `#include`s `Message.hpp` and does `std::visit` on the variant must handle new alternatives. The key touchpoints are: `AiJson.hpp` (DTO visitor), `JsonlSessionStore.cpp` (redaction visitor), `AgentLoop.cpp` (message processing), and test files.
- **Error propagation:** DTO deserialization of unknown role strings continues to return errors as before. New role strings are additive and backward-compatible.
- **State lifecycle risks:** Minimal — the new types are passive values with no internal state or resource ownership.
- **API surface parity:** No public header API changes beyond `MessageVariant` expansion. No new headers created. No existing function signatures change.
- **Integration coverage:** The `convertToLlm` hook in `AsyncAgentOptions` already accepts `std::vector<MessageVariant>` — the new types automatically flow through it once they're in the variant.
- **Unchanged invariants:** `AsyncAgentTool` interface, `AsyncExecutionEnv` interface, `ProviderRegistry`, `StreamingChatClient`, CMake dependency direction, and CLI argument parsing are all untouched.

---

## Risks & Dependencies

| Risk | Mitigation |
|------|------------|
| `std::visit` on expanded variant triggers "unhandled alternative" compile errors in files that pattern-match on all alternatives | U3 explicitly adds redaction cases; U4 test suites catch any remaining missing visitors. The blast radius is limited to files that exhaustively visit `MessageVariant`. |
| `MessageDto` field proliferation makes the DTO struct unwieldy | Fields are all `std::optional`, keeping the DTO flat. If this becomes a problem, future plans can split DTOs per type — but for 4 types, the flat approach is simpler. |
| Test `AsyncAgentLoopTest.cpp:749` ("convertToLlm hook filters non-LLM messages") breaks because it pattern-matches on old variant size | The test's intent (filtering non-LLM messages) aligns with adding new types. U4 updates the test to explicitly include/exclude new types. |

---

## Sources & References

- **Roadmap:** `docs/plans/2026-06-16-001-refactor-pi-cpp-parity-todo.md` (T5 item 2)
- **T5 implementation plan:** `docs/plans/2026-06-20-001-feat-t5-tool-config-parity-plan.md` (deferred T5 item 2)
- **Contract inventory:** `docs/plans/2026-06-16-003-refactor-pi-cpp-contract-inventory.md`
- **pi messages.ts:** `pi:packages/coding-agent/src/core/messages.ts`
- **pi session format:** `pi:packages/coding-agent/docs/session-format.md`
- Related code: `include/cch/ai/Message.hpp`, `include/cch/ai/glaze/AiJson.hpp`, `src/harness/session/JsonlSessionStore.cpp`

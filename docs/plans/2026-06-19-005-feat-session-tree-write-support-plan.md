---
title: "feat: Add write support for pi v3 session tree entries"
type: "feat"
status: active
date: "2026-06-19"
origin: "docs/plans/2026-06-16-001-refactor-pi-cpp-parity-todo.md"
target_repo: "cpp-coding-harness"
reference_repo: "pi"
---

# feat: Add write support for pi v3 session tree entries

**Target repo:** `cpp-coding-harness`. Paths without a repo label are relative to this repository.
**Reference repo:** `pi`. Paths prefixed with `pi:` are relative to the sibling/reference pi checkout.

## Summary

Add JSONL write support for the 9 non-message pi v3 tree entry types (model change, thinking level change, active tools change, custom, custom message, label, compaction, branch summary, session info) that have write-equivalent pi contracts, switch the session header from v2 `"header"` to v3 `"session"`, generate pi-compatible 8-char random hex entry IDs, and remove the resume gate that currently blocks sessions containing tree entries. The read side already parses all entry types; this plan completes the write side while preserving backward compatibility with existing linear sessions.

---

## Requirements

- R1. New sessions write a v3 header (`"type":"session"`, `version: 3`) with pi-canonical fields (`id`, `timestamp`, `cwd`); existing v2 headers remain loadable.
- R2. Every tree entry type defined in `SessionEntryKind` (except `Header`, `Message`, `Unknown`, and `Leaf`) has a corresponding write path that produces JSONL matching pi's field names and shapes. `Leaf` is C++-internal only and remains parse-only.
- R3. Entry IDs are 8-char random hex (matching pi convention), replacing the current sequential `"mN"` format for all new entries.
- R4. Each tree entry carries `id`, `parentId` (nullable), and `timestamp` fields per the `SessionEntryBase` contract in `pi:packages/coding-agent/docs/session-format.md`.
- R5. Tree entry payloads are written as-is without key-based redaction — tree entries contain metadata (model names, thinking levels, tool names, session names, summaries), not user secrets. Callers are responsible for redacting sensitive data before passing it to append methods.
- R6. `open_existing()` succeeds on sessions containing tree entries; the resume gate (`parse_only_tree_kind()`) is removed.
- R7. Existing `append(MessageVariant)` and linear session load/resume behavior is unchanged — backward compatibility is preserved.
- R8. Round-trip tests cover write→load for every supported entry type.

---

## Scope Boundaries

- Backward compatibility with existing v2 linear sessions is preserved: v2 sessions load correctly, v2 `"header"` is recognized, and `append(MessageVariant)` continues to work.
- This plan does NOT implement tree-based context reconstruction (`buildSessionContext()`), branch navigation, or fork/clone operations — those are deferred.
- This plan does NOT change how the agent loop or CLI runtime produces tree entries — only the session store's ability to persist them.
- The `Leaf` entry kind (C++ internal only, no pi equivalent) is parse-only; write support is excluded.

### Deferred to Follow-Up Work

- Tree context reconstruction and branch navigation: separate T4 follow-up plan after entry write compatibility is proven.
- Agent loop integration: the loop does not yet emit tree entry events; wiring the agent loop to call these write methods is a T3/T5 follow-up.

---

## Context & Research

### Relevant Code and Patterns

- `src/harness/session/JsonlSessionStore.cpp` — anonymous-namespace DTO pattern (`WriteHeaderDto`, `ReadHeaderDto`, `MessageEntryDto`), Glaze typed serialization, POSIX append with `O_NOFOLLOW`, redaction pipeline.
- `include/cch/harness/session/SessionEntry.hpp` — `SessionEntryKind` enum (13 values), `SessionEntry` struct with tree fields, `LoadedSession` dual `messages`/`entries` vectors.
- `include/cch/ai/glaze/AiJson.hpp` — message DTO serialization; the pattern of separating DTO structs from domain types.
- `tests/harness/session/JsonlSessionStoreTest.cpp` — Catch2 test patterns, `TempWorkspace` RAII fixture, `make_private()` helper, manual JSONL construction for v3 tree test input.
- `include/cch/util/Json.hpp` — `write_json()` for arbitrary `JsonValue` serialization, `read_json()` for parsing.

### External References

- `pi:packages/coding-agent/docs/session-format.md` — canonical pi v3 entry type specifications: field names, tree structure, entry base contract (`id`, `parentId`, `timestamp`), per-type fields.
- `docs/plans/2026-06-16-001-refactor-pi-cpp-parity-todo.md` — T4 session tree items and done criteria.
- `docs/plans/2026-06-16-003-refactor-pi-cpp-contract-inventory.md` — session entry contract classification and per-type notes.

---

## Key Technical Decisions

- **Header format: v3 `"session"` for new sessions, v2 `"header"` loadable.** The read-side `from_dto()` already branches on `dto.type == "session"`, so adding v3 write is a one-line change. V2 sessions remain loadable without migration. Provider and model metadata are retained in the v3 header as extra fields (pi v3 headers omit them, but extra fields are harmless — JSON parsers ignore unknown keys, and `LoadedSession::metadata` still needs them).
- **Entry ID format: 8-char random hex via `<random>`.** Replaces sequential `"mN"` for new entries. The old format is still readable (IDs are strings, not structured). No collision detection is needed — 8 hex chars = 4 billion namespace. The existing `MessageEntryDto` retains its `entryId` field name (not renamed to `id`) for backward compatibility with existing JSONL sessions; new tree-entry DTOs use `id` as the canonical field name matching pi convention. The read-side `populate_tree_fields()` checks both `entryId` and `id` — no parse breakage.
- **Per-type DTO structs in anonymous namespace.** Each tree entry type gets a Glaze-reflectable DTO mirroring pi field names exactly (`id`, `parentId`, `timestamp`, plus type-specific fields). This follows the existing `MessageEntryDto` pattern and keeps serialization in the implementation layer.
- **Per-type public append methods.** Following pi's `SessionManager` API convention: `append_model_change()`, `append_thinking_level_change()`, etc. Each method constructs the type-specific DTO, delegates to a shared internal `write_entry_line()`, and redacts via `redact_json_value()`.
- **`next_entry_id_` becomes entry count, not ID source.** Random hex IDs eliminate the need for a sequential counter as ID generator. The field remains as total entry count (for debugging) but is no longer used for ID generation. `open_existing()` does not need to seed it for ID uniqueness.
- **Redaction: caller responsibility for tree entries, store handles messages only.** The existing `redacted_message()` pipeline continues for `append(MessageVariant)`. Tree entry metadata (model names, thinking levels, tool names, token counts, session names, summaries) does not contain user secrets; key-based redaction causes false positives on legitimate fields like `tokensBefore` (contains "token"). The `custom_entry` and `custom_message_entry` methods accept `util::JsonValue` — callers are responsible for redacting any sensitive data before calling these methods.
- **Resume gate: removed entirely.** The `parse_only_tree_kind()` check in `open_existing()` is deleted. Since tree context reconstruction is deferred, callers are responsible for inspecting `LoadedSession::entries` to decide whether tree-aware resume is appropriate. The store no longer enforces this.

---

## Open Questions

### Resolved During Planning

- **Should the header switch unconditionally to v3?** Yes — new sessions write `"type":"session"` with `version: 3`. Read side already handles both.
- **Should `append(MessageVariant)` also use random hex IDs?** Yes — all new entries use random hex, regardless of type or session version.
- **Should `Leaf` entries get write support?** No — `Leaf` is C++ internal, has no pi equivalent, and stays parse-only.
- **Should per-type append accept `parent_id`?** Yes — every append method accepts an optional `parent_id` (nullable for the first tree entry after the header).
- **Should the store generate timestamps or accept them?** Generate them internally using ISO 8601 format matching pi convention.

### Deferred to Implementation

- Exact `<random>` engine choice and seeding strategy for ID generation.
- ISO 8601 timestamp formatting details (precision, timezone handling).
- Whether `next_entry_id_` should be renamed to `entry_count_` for clarity.
- Exact Glaze reflection annotations for each DTO struct.

---

## Implementation Units

### U1. Entry ID generation and v3 header

**Goal:** Replace sequential `"mN"` IDs with 8-char random hex, switch `create_new()` to write v3 `"session"` header.

**Requirements:** R1, R3

**Dependencies:** None

**Files:**
- Modify: `src/harness/session/JsonlSessionStore.cpp`
- Test: `tests/harness/session/JsonlSessionStoreTest.cpp`

**Approach:**
- Add a `generate_entry_id()` helper in the anonymous namespace using `<random>` to produce 8-char hex strings.
- Update `to_dto(entry_id, message)` in the message append path to call `generate_entry_id()` instead of formatting `"m" + std::to_string(n)`.
- Change `WriteHeaderDto::type` from `"header"` to `"session"` and `version` from `2` to `3`.
- Update `WriteHeaderDto` fields: add `cwd` (rename from `workspace` to match pi), `timestamp` (rename from `createdAt`), `id` (rename from `sessionId`). Keep the old `ReadHeaderDto` mapping unchanged.
- Remove `next_entry_id_` as ID source; keep the field as entry count only (increment on every append).

**Patterns to follow:**
- `next_entry_id_` usage in existing `append()` (line ~440): replace `"m" + std::to_string(next_entry_id_)` with `generate_entry_id()`.
- `WriteHeaderDto` struct (line ~18): update fields and type/version values.

**Test scenarios:**
- Happy path: New session writes `"type":"session"` with `version: 3`; loading it back produces correct metadata.
- Happy path: Entry IDs in new sessions are 8-char hex strings, not `"m1"`, `"m2"`.
- Edge case: Old v2 `"type":"header"` sessions still load correctly (no regression in existing tests).

**Verification:**
- Existing `[u7]` tests pass without modification.
- New `create_new()` output loads back with correct `session_id`, `workspace`, `version` detection.

---

### U2. Write DTOs and internal write pipeline

**Goal:** Define Glaze-reflectable DTO structs for all 10 non-message entry types and implement a shared internal `write_entry_line()` helper.

**Requirements:** R2, R3, R4

**Dependencies:** U1

**Files:**
- Modify: `src/harness/session/JsonlSessionStore.cpp`
- Test: `tests/harness/session/JsonlSessionStoreTest.cpp`

**Approach:**
- Define DTO structs in the anonymous namespace, one per entry type. Each struct mirrors pi field names:

  | DTO struct | `type` string | Extra fields beyond `id`, `parentId`, `timestamp` |
  |---|---|---|
  | `ModelChangeDto` | `"model_change"` | `provider`, `modelId` |
  | `ThinkingLevelChangeDto` | `"thinking_level_change"` | `thinkingLevel` |
  | `ActiveToolsChangeDto` | `"active_tools_change"` | `tools` (JSON array of tool names) |
  | `CustomDto` | `"custom"` | `customType`, `data` (`glz::raw_json`) |
  | `CustomMessageDto` | `"custom_message"` | `customType`, `content`, `display` (bool), `details` (optional, `glz::raw_json`) |
  | `LabelDto` | `"label"` | `targetId`, `label` (optional, `std::nullopt` clears) |
  | `CompactionDto` | `"compaction"` | `summary`, `firstKeptEntryId`, `tokensBefore`, `details` (optional, `glz::raw_json`), `fromHook` (optional) |
  | `BranchSummaryDto` | `"branch_summary"` | `fromId`, `summary`, `details` (optional, `glz::raw_json`), `fromHook` (optional) |
  | `SessionInfoDto` | `"session_info"` | `name` |
  | `LeafDto` | `"leaf"` | (parse-only, no write DTO needed) |

- Each DTO struct is standalone with `type`, `id`, `parentId`, `timestamp` as inline fields — Glaze reflection operates on the concrete struct, so a composed base would produce nested JSON without additional flattening machinery. Inline repetition of 4 common fields across 9 DTOs is simpler and matches the existing `WriteHeaderDto`/`MessageEntryDto` pattern.
- Implement `write_entry_line()`: takes a const reference to any DTO, converts to `util::JsonValue`, applies `redact_json_value()` to the value before serialization (matching U3's described pipeline), then serializes via `glz::write_json()`, appends to file with POSIX `::write()`/`::fsync()` (or `std::ofstream` fallback), increments `next_entry_id_`.
- Extract the file append/fsync logic from `append()` into a reusable `append_line_to_file(const std::string& line)` helper.

**Patterns to follow:**
- `MessageEntryDto` struct (line ~47): Glaze-reflectable aggregate with `type` string.
- `to_dto()` conversion pattern (line ~114): construct DTO, serialize, append.
- POSIX append block in `append()` (line ~440-480): `O_WRONLY | O_APPEND | O_CREAT | O_NOFOLLOW`, `::write()`, `::fsync()`.

**Test scenarios:**
- (Tests deferred to U5; this unit is internal infrastructure.)

**Verification:**
- Compilation succeeds with all DTO structs defined and Glaze-reflectable.
- `write_entry_line()` can be called from downstream U3 methods.

---

### U3. Public append API

**Goal:** Add per-type append methods to `JsonlSessionStore`'s public interface, with redaction and timestamp generation.

**Requirements:** R2, R4, R5

**Dependencies:** U2

**Files:**
- Modify: `include/cch/harness/session/JsonlSessionStore.hpp`
- Modify: `src/harness/session/JsonlSessionStore.cpp`
- Test: `tests/harness/session/JsonlSessionStoreTest.cpp`

**Approach:**
- Add public methods to `JsonlSessionStore`:

  ```cpp
  util::ExpectedVoid append_model_change(std::optional<std::string> parent_id,
                                         std::string provider,
                                         std::string model_id);
  util::ExpectedVoid append_thinking_level_change(std::optional<std::string> parent_id,
                                                   std::string thinking_level);
  util::ExpectedVoid append_active_tools_change(std::optional<std::string> parent_id,
                                                 std::vector<std::string> tools);
  util::ExpectedVoid append_custom_entry(std::optional<std::string> parent_id,
                                          std::string custom_type,
                                          util::JsonValue data);
  util::ExpectedVoid append_custom_message_entry(std::optional<std::string> parent_id,
                                                  std::string custom_type,
                                                  std::string content,
                                                  bool display,
                                                  std::optional<util::JsonValue> details);
  util::ExpectedVoid append_label_change(std::optional<std::string> parent_id,
                                          std::string target_id,
                                          std::optional<std::string> label);
  util::ExpectedVoid append_compaction(std::optional<std::string> parent_id,
                                        std::string summary,
                                        std::string first_kept_entry_id,
                                        std::size_t tokens_before,
                                        std::optional<util::JsonValue> details,
                                        std::optional<bool> from_hook);
  util::ExpectedVoid append_branch_summary(std::optional<std::string> parent_id,
                                            std::string from_id,
                                            std::string summary,
                                            std::optional<util::JsonValue> details,
                                            std::optional<bool> from_hook);
  util::ExpectedVoid append_session_info(std::optional<std::string> parent_id,
                                          std::string name);
  ```

- Each method: generates `id` via `generate_entry_id()`, generates ISO 8601 `timestamp`, constructs the corresponding DTO, calls `write_entry_line()`, returns `util::ExpectedVoid`.
- Timestamps generated via `<chrono>` + `std::put_time` or `std::format` (C++23) in ISO 8601 format.
- `parent_id` is `std::optional<std::string>` — `std::nullopt` writes `null` for the first tree entry.
- `append_custom_entry()` and `append_custom_message_entry()` accept `util::JsonValue` for `data`/`details` — redaction is applied to these via `redact_json_value()` before serialization.

**Patterns to follow:**
- Method signatures in `JsonlSessionStore.hpp`: `[[nodiscard]] util::ExpectedVoid append_*(...)`.
- Public header stays free of Glaze types, DTO structs, and implementation details.

**Test scenarios:**
- (Integration tests deferred to U5; unit-level correctness verified through the round-trip tests there.)

**Verification:**
- All methods compile and link.
- Public header includes no Glaze headers, no DTO types.

---

### U4. Resume gate removal and entry counting fix

**Goal:** Remove the `parse_only_tree_kind()` gate in `open_existing()` so sessions with tree entries can be resumed. Fix `next_entry_id_` seeding to count all entries.

**Requirements:** R6, R7

**Dependencies:** U1

**Files:**
- Modify: `src/harness/session/JsonlSessionStore.cpp`
- Test: `tests/harness/session/JsonlSessionStoreTest.cpp`

**Approach:**
- Delete the `parse_only_tree_kind()` loop and early return in `open_existing()` (lines ~447-453 of the current implementation).
- After removal, `open_existing()` loads all entries without gating on non-message types.
- Update `next_entry_id_` seeding: since IDs are now random hex, `next_entry_id_` is an entry count. Set it to `loaded->entries.size()` (counting all entries including the header) rather than `loaded->messages.size() + 1`.
- Existing `append(MessageVariant)` continues to work after resume — it now uses `generate_entry_id()` instead of sequential counter, so no ID collision risk.

**Patterns to follow:**
- `open_existing()` current structure: load → validate header → check for tree entries → return store. Remove only the tree-entry rejection block.

**Test scenarios:**
- Happy path: Create a session with v3 header + tree entries (model_change, thinking_level_change, then a user message), open via `open_existing()`, append a new message — succeeds without error.
- Regression: Existing `[u8]` test "Glaze JSONL session parses v3 tree metadata entries" previously asserted `open_existing()` fails — update the assertion to expect success after U4.
- Edge case: Session with only header and tree entries (no messages) can be opened — `loaded->messages` is empty but `loaded->entries` has the tree structure.

**Verification:**
- Updated `[u8]` test passes with `open_existing()` succeeding.
- New round-trip tests (U5) that call `open_existing()` after writing tree entries all pass.

---

### U5. Round-trip tests

**Goal:** Add comprehensive write→load round-trip tests for every supported entry type, v3 header format, and resume-after-tree-entries.

**Requirements:** R1, R2, R3, R4, R5, R8

**Dependencies:** U3, U4

**Files:**
- Modify: `tests/harness/session/JsonlSessionStoreTest.cpp`

**Approach:**
- Add a `[u9]` test case group covering:

  1. **V3 header round-trip:** `create_new()` → `load()` → verify `metadata.session_id`, `metadata.workspace`, header `version` detection.
  2. **Entry ID format:** Verify generated IDs are 8-char hex (regex or length + hex check).
  3. **ModelChange round-trip:** `append_model_change(null, "openai", "gpt-4o")` → load → verify `kind == ModelChange`, `entry_id` is hex, `parent_id` is null, payload contains `provider`/`modelId`.
  4. **ThinkingLevelChange round-trip:** `append_thinking_level_change("parent1", "high")` → load → verify `parent_id == "parent1"`, `thinkingLevel == "high"`.
  5. **ActiveToolsChange round-trip:** Write with `tools = ["read", "write"]` → load → verify `tools` array in payload.
  6. **Custom entry round-trip:** Write with `customType = "my-ext"`, `data = {"count": 42}` → load → verify payload fields.
  7. **CustomMessage entry round-trip:** Write with `display = true`, `content = "hello"`, `details = {"key": "val"}` → load → verify all fields.
  8. **Label entry round-trip:** Write label then clear it (null label) → load → verify both states.
  9. **Compaction entry round-trip:** Write with `summary`, `firstKeptEntryId`, `tokensBefore`, `details`, `fromHook` → load → verify all fields.
  10. **BranchSummary entry round-trip:** Write with `fromId`, `summary`, `details`, `fromHook` → load → verify.
  11. **SessionInfo entry round-trip:** Write session name → load → verify `name` in payload.
  12. **Mixed tree + messages round-trip:** Write header → model_change → thinking_level_change → user message → assistant message → load → verify entries are in correct order with correct kinds and tree linkage.
  13. **Redaction of tree entries:** Write a `CustomDto` with secret-like keys in `data` → verify raw file contains `[REDACTED]` for secret values, while safe values are preserved.
  14. **Resume after tree entries:** Create session → write model_change → write user message → `open_existing()` → append new message → `load()` → verify all entries preserved and new entry appended correctly with proper ID.

- Use the existing `TempWorkspace` + `make_private()` pattern.
- For entry ID validation, use a helper like `bool is_hex8(const std::string& s)` checking length == 8 and all chars in `[0-9a-f]`.

**Patterns to follow:**
- Existing `[u8]` test "Glaze JSONL session parses v3 tree metadata entries" — manual JSONL construction for input, `load()` verification.
- Existing `[u7]` redaction test — raw file content check for `[REDACTED]` presence and secret absence.
- `metadata_for()` and `user_message()` helpers in the test file.

**Test scenarios:**
- Happy path: Each entry type writes and loads back with correct kind, tree fields, and type-specific fields. (14 sub-scenarios enumerated above.)
- Edge case: `parent_id` as `std::nullopt` writes `null` in JSON.
- Edge case: Session with zero messages (only tree entries and header) loads with empty `messages` vector but populated `entries`.
- Error path: Malformed tree entry JSONL (wrong type field) produces load error with line context.
- Integration: Full mixed session (header + multiple tree entries + messages) round-trips correctly with all entry ordering preserved.

**Verification:**
- All new `[u9]` tests pass.
- Existing `[u7]` and `[u8]` tests pass (with `[u8]` updated per U4 for the resume gate change).
- `ctest -R "session"` passes the full suite.

---

## System-Wide Impact

- **Interaction graph:** `JsonlSessionStore` is called from `AsyncCliRuntime::run_prompt` (the sole `append()` caller today) and from `main.cpp` session lifecycle code. New append methods are additive — no existing callers need updating.
- **Error propagation:** All new append methods return `util::ExpectedVoid` with `ErrorCode::Session` on failure, consistent with existing error flow.
- **State lifecycle risks:** The `next_entry_id_` field semantics change from "next sequential ID" to "total entry count." No external code reads this field directly. The `open_existing()` seeding logic is the only consumer.
- **API surface parity:** New public methods on `JsonlSessionStore` follow the same pattern as the existing `append()`. No new headers or dependencies are introduced.
- **Integration coverage:** The only cross-layer scenario is agent loop → store → JSONL file → store → `LoadedSession`. The round-trip tests in U5 cover this end-to-end for all entry types.
- **Unchanged invariants:** `append(MessageVariant)` behavior is preserved for existing callers. `load()` continues to handle both v2 and v3 sessions. `LoadedSession` struct shape is unchanged. Private permissions, symlink rejection, and `O_NOFOLLOW` safety are untouched.

---

## Risks & Dependencies

| Risk | Mitigation |
|------|------------|
| ID collision with 8-char random hex | 8 hex chars = 4B namespace; collision probability is negligible for session-scale entry counts. No collision detection code needed. |
| V3 header breaks downstream consumers that inspect raw JSONL | V3 header change is additive — read side handles both formats. Downstream consumers of `LoadedSession::metadata` (not raw JSONL) are unaffected. |
| `next_entry_id_` rename/repurpose breaks a hidden dependency | Grep confirms `next_entry_id_` is private and only used within `JsonlSessionStore.cpp`. No external exposure. |
| Timestamp format mismatch with pi convention | Pi uses ISO 8601 with milliseconds + `Z` suffix. C++23 `std::chrono` + `std::format` can produce this exactly. Verify against pi-generated session files in tests. |

---

## Sources & References

- **Origin document:** `docs/plans/2026-06-16-001-refactor-pi-cpp-parity-todo.md` (T4 session tree items)
- **Contract inventory:** `docs/plans/2026-06-16-003-refactor-pi-cpp-contract-inventory.md` (session entry classification)
- **Predecessor plan:** `docs/plans/2026-06-16-002-refactor-pre-implementation-cleanup-plan.md` (U8 parse-only tree entry support)
- **pi session format:** `pi:packages/coding-agent/docs/session-format.md`
- **Session entry types:** `include/cch/harness/session/SessionEntry.hpp`
- **Session store impl:** `src/harness/session/JsonlSessionStore.cpp`
- **Session store tests:** `tests/harness/session/JsonlSessionStoreTest.cpp`
- **Message DTO serialization:** `include/cch/ai/glaze/AiJson.hpp`

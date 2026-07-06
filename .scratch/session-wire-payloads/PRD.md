# PRD: Stop Leaking Session Wire Payloads

Status: in-progress

## Problem Statement

Session tree behavior now depends on parsed JSON payloads that still expose the
session file's wire field names to domain logic. The serializer parses JSONL into
session entries, but known non-message entries still carry most of their useful
state as generic JSON. As a result, tree navigation, context reconstruction,
leaf restoration, and many tests know details such as `modelId`,
`thinkingLevel`, `firstKeptEntryId`, `fromId`, `customType`, and `targetId`.

That makes the session module shallower than intended. Serialization machinery
crosses the seam into `SessionTree`, and tests that should protect domain
behavior instead assert JSON object shape. This also makes future session-format
compatibility harder, because changing a wire field or accepting an alternate
legacy field requires changes outside the serializer.

## Solution

Make the session serializer produce typed passive session entry values for every
known session entry kind. The session tree and session-store behavior should
consume these typed values instead of inspecting generic JSON payloads. Wire
field names, Glaze DTOs, raw JSON, raw lines, and compatibility parsing should
stay inside the serialization and diagnostics boundary.

Known entries should remain append-only JSONL on disk and compatible with the
current pi-shaped session format. Unknown entries should still be preserved as
unknown lines or diagnostics-only state. The visible user behavior of resume,
branch continuation, compaction-aware context reconstruction, SDK topology
gating, and simple linear session append should not change.

## User Stories

1. As a session module maintainer, I want known session entries represented as typed values, so that tree logic does not parse JSON field names.
2. As a session module maintainer, I want wire field names owned by the serializer, so that JSONL compatibility changes stay localized.
3. As a future implementer, I want `SessionTree` to read entry meaning from passive value fields, so that branch and compaction code is easier to reason about.
4. As a test author, I want store tests to assert typed loaded entries, so that tests protect behavior rather than generic JSON shape.
5. As a test author, I want dedicated wire-format tests where JSON field names matter, so that serializer compatibility remains explicitly covered.
6. As a branch user, I want leaf restoration to keep working, so that persisted branch position still survives process restarts.
7. As a branch user, I want invalid leaf fallback to keep working, so that stale leaf markers do not break resume.
8. As a compaction user, I want compacted context reconstruction to keep using the closest active compaction, so that old messages stay out of resumed model context.
9. As a compaction user, I want compaction summary and token metadata parsed once, so that context code does not duplicate wire parsing.
10. As a custom-message user, I want custom message entries to still become model-visible custom messages, so that extension-injected context remains available.
11. As a branch-summary user, I want branch summary entries to still become branch summary messages, so that abandoned branch context is preserved.
12. As a provider configuration maintainer, I want model and thinking-level changes loaded as typed context values, so that future provider logic can consume them without parsing JSON.
13. As a session-format maintainer, I want unknown entries preserved without giving them typed semantics, so that forward compatibility remains safe.
14. As a security maintainer, I want redaction behavior to remain unchanged, so that typed entry work does not leak secrets into session files.
15. As an SDK maintainer, I want topology and resume decisions to keep consuming the same session semantics, so that SDK v1 behavior does not regress.
16. As a CLI user, I want session resume behavior to stay the same, so that this refactor is not visible as a workflow change.
17. As a JSON/RPC client, I want loaded sessions to keep the same observable history, so that machine-readable integrations do not need to adapt.
18. As a code reviewer, I want raw payload access treated as diagnostics-only for known entries, so that future changes do not reintroduce serializer leakage.
19. As a future agent, I want the domain language to distinguish wire DTOs from session entry values, so that implementation decisions stay anchored to the intended seam.
20. As a maintainer of pi parity, I want C++ session values to remain idiomatic passive C++ aggregates, so that parity does not become mechanical TypeScript translation.

## Implementation Decisions

- The primary design seam is between session JSONL serialization and session domain logic.
- The serializer owns DTOs, Glaze mappings, wire field names, raw JSON conversion, legacy field compatibility, and malformed-entry error reporting.
- Known loaded entries should carry typed passive value state in addition to their entry kind, entry ID, parent ID, and optional leaf ID.
- Typed values should cover model changes, thinking-level changes, active-tools changes, custom entries, custom-message entries, labels, compactions, branch summaries, session info, and leaf markers.
- Message entries should continue to expose the parsed provider-neutral message value.
- Unknown entries should remain unknown. They may keep raw line or generic payload information for diagnostics and forward compatibility, but they must not participate in tree semantics as known entries.
- `SessionTree` should use typed session entry values for leaf restoration, effective model/thinking state, compaction context reconstruction, branch summary conversion, and custom message conversion.
- Session-store append and resume-adjacent behavior should use typed leaf values rather than looking up `targetId` from a generic JSON object.
- Raw payload access may remain available only as a diagnostics or unknown-entry escape hatch. Known-entry domain code and behavior tests should not depend on it.
- The on-disk JSONL session format must stay compatible with the current pi-shaped session entries.
- This work should preserve backward-compatible parsing for existing sessions already written by this repository.
- This work should not change provider/model resolution. Model and thinking-level entries are parsed as passive session state only.
- This work should not create a broad public session-tree API. Keep capability-heavy navigation inside the session module.
- This work should not move Glaze DTOs, schema conversion helpers, or parsing helpers into domain-facing APIs.
- If public headers change, they should expose passive aggregate values, not serializer DTOs or implementation helpers.

## Testing Decisions

- Good tests should assert externally visible session behavior and typed entry meaning, not internal generic JSON object traversal.
- The primary validation seam is loading a JSONL session into typed session entries and then building tree context from those entries.
- Serializer/store tests should cover every known session entry kind and assert that the loaded typed value matches the appended or parsed data.
- Serializer wire-format tests should still check critical JSONL compatibility, including current field names, legacy accepted fields where supported, and unknown-entry preservation.
- Session tree tests should continue to cover leaf restoration, invalid leaf fallback, model and thinking-level extraction, compaction context reconstruction, branch summary conversion, and custom message conversion.
- Store/resume tests should cover branch continuation after resume, proving leaf persistence still works after removing generic `targetId` lookup from domain code.
- Existing SDK resume topology tests should continue to pass, proving the refactor does not change SDK v1 fail-closed behavior.
- Architecture tests should run if public session headers or CMake public/private dependency boundaries change.
- Focused validation should include session store tests, session tree tests, resume lifecycle tests, SDK resume topology tests if touched, and architecture tests for public-boundary changes.

## Out of Scope

- Changing the JSONL session format.
- Adding new session entry kinds.
- Adding branch selection CLI, REPL, or RPC commands.
- Changing compaction generation behavior.
- Changing branch summary generation behavior.
- Making model or thinking-level entries affect provider/model resolution.
- Reworking SDK v1 non-linear session support.
- Moving runtime-only message types out of AI contracts.
- Reworking project resource loading.
- Retiring the legacy execution surface.

## Further Notes

- Source candidate: `/tmp/architecture-review-20260705-175740.html`, `#session-wire`.
- Relevant pi references: `packages/coding-agent/docs/session-format.md`, `packages/coding-agent/src/core/session-manager.ts`, and `packages/agent/src/harness/types.ts`.
- This PRD follows the repository guardrail that data is passive value state and serialization machinery stays local to implementation layers.
- The expected module shape is a deeper session serializer/domain boundary: more wire compatibility behind the serializer, less JSON-key knowledge in tree logic and behavior tests.

## Implementation Progress

- Issue 01 implemented by `e10feaa Introduce typed session entry values`.
- Issues 02 and 03 are ready for fresh implementation sessions.
- Issue 04 remains blocked by issues 02 and 03.

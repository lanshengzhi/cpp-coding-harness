# Introduce typed session entry values

Status: implemented

## Parent

.scratch/session-wire-payloads/PRD.md

## What to build

Make the session load seam produce typed passive values for every known
non-message session entry kind. A loaded known entry should carry its domain
meaning directly, instead of requiring callers to inspect generic JSON payloads.

The slice should preserve JSONL compatibility, unknown-entry preservation,
redaction behavior, existing message loading, and existing metadata loading.
Raw payload or raw-line state may remain available for diagnostics and unknown
entries, but known-entry behavior should be accessible through typed values.

## Acceptance criteria

- [x] Loaded known entries expose typed passive values for model changes, thinking-level changes, active-tools changes, custom entries, custom-message entries, labels, compactions, branch summaries, session info, and leaf markers.
- [x] Message entries continue to expose parsed provider-neutral message values.
- [x] Unknown entries remain preserved as unknown diagnostics or unknown lines without gaining false typed semantics.
- [x] JSONL wire field names and Glaze DTO details stay owned by the serializer boundary.
- [x] Existing sessions written by the repository still load successfully.
- [x] Store tests assert typed entry values for each known entry kind.
- [x] Critical wire-format compatibility checks remain covered at the serializer/store boundary.
- [x] Secret redaction behavior remains unchanged.
- [x] Focused session store tests pass.

## Blocked by

None - can start immediately

## Comments

- Implemented by `e10feaa Introduce typed session entry values`.
- Rechecked with `./build/cpp_harness_tests "[harness][session]"`.

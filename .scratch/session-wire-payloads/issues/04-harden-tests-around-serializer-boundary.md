# Harden tests around the serializer boundary

Status: implemented

## Parent

.scratch/session-wire-payloads/PRD.md

## What to build

Finish the session-wire cleanup by making tests express the intended boundary.
Known-entry tests should assert typed loaded values and user-visible session
behavior. Tests that care about exact JSON field names should be clearly scoped
to the serializer or wire-format boundary. This issue should also remove or
rename any stale test helpers or comments that imply generic payload access is
the primary domain interface.

This issue is the final integration pass for the PRD. It should verify that the
session module no longer relies on raw wire payloads in known-entry domain
logic, while still preserving diagnostics and unknown-entry compatibility.

## Acceptance criteria

- [x] Store tests for known session entries assert typed entry values instead of generic JSON payload shape.
- [x] Wire-format tests still explicitly protect the current JSONL field names where compatibility matters.
- [x] Session tree tests assert behavior through context reconstruction, branch navigation, and leaf restoration rather than raw payload fields.
- [x] Unknown-entry preservation remains covered.
- [x] Stale comments or helper names that describe the old payload-driven seam are updated where touched.
- [x] A search for known-entry wire-field lookups in session domain logic shows none outside the serializer or intentional wire-format tests.
- [x] Architecture tests pass if public session headers or dependency boundaries changed.
- [x] Focused session store, session tree, resume lifecycle, and relevant SDK resume tests pass.

## Blocked by

- .scratch/session-wire-payloads/issues/02-move-session-tree-context-to-typed-values.md
- .scratch/session-wire-payloads/issues/03-move-leaf-resume-and-append-continuation-off-wire-payloads.md

## Comments

### 2026-07-06 Implementation

- Moved SessionTree tests that previously used wire-shaped setup onto typed
  `SessionEntry` values, including timestamp propagation, context
  reconstruction, and leaf restoration coverage.
- Kept exact JSONL field-name assertions in clearly tagged serializer/wire
  tests, and strengthened raw JSON parser setup to assert typed loaded values.
- Verified unknown-entry preservation still runs in the focused session-store
  slice.
- Validation:
  - `cmake --build build --target cpp_harness_tests -j 4`
  - `./build/cpp_harness_tests "[harness][session][tree]"`
  - `./build/cpp_harness_tests "[harness][session][u7]"`
  - `./build/cpp_harness_tests "[harness][session][u9]"`
  - `./build/cpp_harness_tests "[harness][session][wire]"`
  - `./build/cpp_harness_tests "[coding-agent][runtime][session]"`
  - `./build/cpp_harness_tests "[sdk][u3][resume-topology]"`
  - `rg -n "modelId|thinkingLevel|firstKeptEntryId|fromId|customType|targetId|activeToolNames" src/harness/session include/cch/harness/session tests/harness/session tests/coding_agent/runtime tests/coding_agent/SdkSessionTest.cpp`
  - `ctest --test-dir build --output-on-failure`
- Local two-axis code review completed against `HEAD`; no standards or spec
  findings.

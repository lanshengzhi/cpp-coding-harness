# Move leaf resume and append continuation off wire payloads

Status: implemented

## Parent

.scratch/session-wire-payloads/PRD.md

## What to build

Update leaf restoration, invalid leaf fallback, and branch-continuation append
behavior so leaf target information comes from typed leaf entry values rather
than generic JSON payload fields. Resuming from a branch and then continuing the
conversation should still make newly appended messages the next resume point.

This slice protects the branch-resume persistence behavior introduced by the
session resume module work while removing the remaining wire-field dependency
from the leaf path.

## Acceptance criteria

- [x] Valid persisted leaf markers restore the active branch using typed leaf values.
- [x] Invalid persisted leaf markers fall back to the last valid navigable entry using typed entry state.
- [x] Opening an existing session determines the active append parent without generic JSON target lookup.
- [x] Appending after branch resume still persists a new leaf marker for the appended message.
- [x] A later resume includes continuation messages instead of snapping back to the old leaf.
- [x] Generic JSON payload lookup is not used for known leaf restoration or branch-continuation append behavior.
- [x] Existing SDK resume topology behavior is unchanged.
- [x] Focused resume lifecycle, session tree, and SDK resume topology tests pass when relevant.

## Blocked by

None - issue 01 is implemented.

## Comments

### 2026-07-06 Implementation

- Added a private typed leaf selection helper shared by `SessionTree` and `JsonlSessionStore::open_existing`.
- Covered stale latest leaf fallback, typed leaf-vs-payload restoration, and append continuation parent/leaf persistence.
- Validation:
  - `cmake --build build --target cpp_harness_tests -j 4`
  - `./build/cpp_harness_tests "SessionTree ignores a stale latest leaf marker and falls back to last navigable entry"`
  - `./build/cpp_harness_tests "open_existing appends after stale latest leaf marker at last navigable entry"`
  - `./build/cpp_harness_tests "SessionTree restores leaf from typed value instead of payload target field"`
  - `./build/cpp_harness_tests "[harness][session][tree]"`
  - `./build/cpp_harness_tests "[harness][session][u9]"`
  - `./build/cpp_harness_tests "[coding-agent][runtime][session]"`
  - `./build/cpp_harness_tests "[sdk][u3][resume-topology]"`
  - `ctest --test-dir build --output-on-failure`
- Local code review completed against `HEAD`; no standards or spec findings.

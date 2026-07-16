# Gate SDK v1 resume through SessionTopology

Category: enhancement
Status: implemented

## Parent

.scratch/session-resume-module/spec.md

## What to build

Make SDK v1 consume `OpenSession`'s `SessionTopology` as the only resume topology decision. SDK assembly should not manually inspect persisted session entries to decide whether a session is linear, branched, or compacted.

Linear active contexts should remain resumable by SDK v1. Branched or compacted active contexts should fail closed with the existing unsupported-topology behavior. Inactive compaction or branch data should not cause SDK v1 to reject an otherwise linear active context.

## Acceptance criteria

- [x] SDK v1 linear resume succeeds through `create_agent_session`.
- [x] SDK v1 rejects branched active topology through `SessionTopology`.
- [x] SDK v1 rejects compacted active topology through `SessionTopology`.
- [x] SDK v1 no longer manually scans persisted JSONL or tree entries for topology.
- [x] Inactive compaction or branch data does not cause SDK v1 to reject a linear active context.
- [x] SDK resume tests cover success and fail-closed paths.
- [x] Provider/model resolution behavior remains unchanged.

## Blocked by

- .scratch/session-resume-module/issues/01-deepen-session-resume-open-session.md

## Comments

- Implemented by `81f9f91 test sdk resume topology gating`.
- Verified with `./build/cpp_harness_tests "[sdk][u3][resume-topology]"`, `./build/cpp_harness_tests "[sdk]"`, `./build/cpp_harness_tests "[coding-agent][runtime][session]"`, `./build/cpp_harness_tests "[harness][session][tree]"`, and `ctest --test-dir build --output-on-failure`.

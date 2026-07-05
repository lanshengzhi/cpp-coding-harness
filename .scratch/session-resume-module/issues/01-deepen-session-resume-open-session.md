# Deepen SessionResume at the open_session seam

Status: implemented

## Parent

.scratch/session-resume-module/PRD.md

## What to build

Make `SessionResume` the single source of resumed context at the `open_session` seam. Resuming a session should return a passive result containing agent-ready history, session metadata, effective model, effective thinking level, store handle, and active-path `SessionTopology`.

Topology classification should live in the session domain, not in runtime assembly. It should describe the active resumed context, so inactive branches or inactive compactions do not make the active context appear branched or compacted. The implementation should also keep `SessionTree` hidden from runtime callers and update any stale documentation that still describes flat transcript replay as the current resume behavior.

## Acceptance criteria

- [x] `open_session` resume returns agent-ready history reconstructed from the active session path.
- [x] `OpenSession` carries passive session metadata without requiring callers to reach through the store for identity data.
- [x] `OpenSession` carries effective model and thinking level as passive context values.
- [x] `OpenSession` carries `SessionTopology` for the active resumed context.
- [x] Topology classification is owned by the session domain, not duplicated in runtime lifecycle or SDK assembly code.
- [x] Linear resume remains linear and returns the same user-visible history as before.
- [x] Compaction resume returns compaction summary plus kept and post-compaction messages for the active path.
- [x] Valid `Leaf` resume restores the selected active branch.
- [x] Invalid persisted `Leaf` target falls back to the last valid entry.
- [x] Inactive branch or compaction data does not incorrectly classify a linear active path as non-linear.
- [x] Actual provider/model resolution is not changed by model or thinking-level context values.
- [x] Any existing docs made stale by tree-aware resume semantics are updated.
- [x] Focused resume, session tree, and documentation checks pass.

## Blocked by

None - can start immediately

## Comments

- Implemented by `ef87f0b Deepen session resume open_session seam`.
- Verified in the final module validation with `./build/cpp_harness_tests "[coding-agent][runtime][session]"`, `./build/cpp_harness_tests "[harness][session][tree]"`, and `ctest --test-dir build --output-on-failure`.

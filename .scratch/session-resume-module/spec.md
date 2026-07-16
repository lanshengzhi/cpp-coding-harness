# Spec: SessionResume Module Completion

Category: enhancement
Status: implemented

## Problem Statement

Session resume behavior is currently split across the runtime lifecycle, session tree, session factory, and append-time persistence paths. The intended direction is for `SessionResume` to be a deep module: callers should ask for an agent-ready resumed context and a passive `SessionTopology`, while tree navigation, active leaf selection, compaction handling, and topology classification stay inside the session domain.

The current implementation moved resume history toward `SessionTree`, but code review found unfinished seams. Topology analysis is still performed outside the session module, branch continuation can be persisted in a way that leaves a stale active leaf, passive session metadata is not returned on `OpenSession`, and the invalid leaf fallback is not covered by the new seam tests. Existing documentation may also describe the old flat transcript resume behavior.

## Solution

Make `SessionResume` the single resume-facing operation for agent sessions. Resuming a session should reconstruct the active path from the persisted session tree, return only agent-ready history and passive resume state to runtime callers, and classify topology from the active resumed context rather than from unrelated stored entries.

After a resumed branch continues, later resumes should continue from the newly appended messages instead of snapping back to an old persisted `Leaf` target. SDK v1 should continue to fail closed for non-linear sessions, but that decision should consume `SessionTopology` from the resume module rather than repeating JSONL or tree-entry scans in the SDK assembly layer.

## User Stories

1. As a CLI user, I want `--resume` to restore the active leaf context, so that branch navigation decisions persist across process restarts.
2. As a REPL user, I want resumed history to include the compacted summary and kept messages, so that the model sees the same context the session tree represents.
3. As a JSON/RPC client, I want resume semantics to match CLI resume semantics, so that integration behavior is predictable across modes.
4. As an agent runtime caller, I want `OpenSession` to provide agent-ready history, so that runtime code does not need to understand session tree internals.
5. As an agent runtime caller, I want `OpenSession` to provide passive session metadata, so that creation-time code does not reach through the store for identity data.
6. As an SDK user, I want SDK v1 to reject branched or compacted sessions consistently, so that unsupported resume shapes do not append incorrectly.
7. As an SDK maintainer, I want SDK v1 to inspect `SessionTopology`, so that topology policy is centralized in one module.
8. As a session module maintainer, I want topology classification to live with tree resume logic, so that parent, child, leaf, and compaction rules are not duplicated in runtime code.
9. As a session module maintainer, I want topology to describe the active resumed context, so that inactive branches do not incorrectly affect SDK resume eligibility.
10. As a branch user, I want an active `Leaf` target to restore the selected branch, so that I can resume where I intentionally navigated.
11. As a branch user, I want an invalid persisted `Leaf` target to fall back to the last valid entry, so that a stale marker does not make resume fail unnecessarily.
12. As a branch user, I want new messages after a branch resume to become the next resume point, so that continuing from a branch survives another restart.
13. As a compaction user, I want the closest compaction on the active path to shape resumed context, so that obsolete pre-compaction messages stay out of model history.
14. As a compaction user, I want compaction on an inactive branch not to affect a linear active path, so that SDK v1 does not reject a session for irrelevant history.
15. As a provider configuration maintainer, I want resumed `model_change` and `thinking_level_change` values to be computed and returned passively, so that future provider resolution can consume them without changing this feature.
16. As a provider configuration maintainer, I do not want this round to change actual provider or model resolution, so that resume architecture can land without changing provider behavior.
17. As a runtime maintainer, I want `AgentSessionRuntime` to remain unaware of `SessionTree`, so that the runtime seam stays narrow.
18. As a test author, I want resume tests at the `open_session` seam, so that behavior is protected where CLI, REPL, and RPC share it.
19. As a test author, I want an end-to-end runtime continuation test after branch resume, so that append-time persistence does not regress the next resume.
20. As a test author, I want an SDK resume rejection test, so that SDK v1 fail-closed behavior remains explicit.
21. As a documentation reader, I want docs to describe tree-aware resume semantics, so that future implementers do not assume flat transcript replay.
22. As a future agent working on sessions, I want the domain language to distinguish `SessionTree`, `SessionResume`, and `SessionTopology`, so that implementation decisions stay anchored to the glossary.

## Implementation Decisions

- `SessionResume` should be treated as the resume-facing domain operation. It owns reconstruction of active-path history, effective context state, topology classification, and passive metadata returned to runtime assembly.
- `SessionTree` remains hidden behind the resume module for runtime callers. Runtime assembly should receive passive values and a store handle, not tree navigation capabilities.
- `OpenSession` remains the return type name and is extended only with necessary passive values: agent-ready history, effective model, effective thinking level, topology, session metadata, and store handle.
- `SessionTopology` should classify the active resumed context as linear, branched, or compacted. Classification should not be based on unrelated inactive entries.
- Compacted topology should be determined by compaction that participates in the active resume context, not merely by the existence of any compaction entry in the stored session.
- Branched topology should represent an active branch or resume shape that SDK v1 cannot append linearly.
- SDK v1 should continue to fail closed for non-linear topology, but the SDK assembly layer should not manually inspect persisted entries to make that decision.
- Branch continuation must update persisted resume position or otherwise ensure that newly appended messages after branch resume become reachable on the next tree resume.
- `model_change` and `thinking_level_change` entries should be computed and returned as passive context values only. They must not change actual provider/model resolution in this spec.
- Existing session store and tree contracts should be reused where possible. New abstractions should be added only if they make `SessionResume` deeper and reduce duplicated topology or navigation knowledge.
- Documentation should be updated only where existing text becomes inaccurate because resume now uses tree-aware context rather than flat transcript replay.
- Current WIP code-review findings are part of this spec's acceptance scope: move topology classification out of runtime lifecycle, update stale docs, improve unclear test helper names where touched, return passive metadata, test invalid leaf fallback, and fix branch continuation persistence.

## Testing Decisions

- Good tests should assert externally visible resume behavior, not implementation details such as private helper names or exact internal scan loops.
- The primary test seam is `open_session`, because CLI, REPL, JSON/RPC, and runtime assembly share this resume boundary.
- `open_session` tests should cover linear resume, compaction resume, active leaf resume, invalid leaf fallback, active-path topology, passive session metadata, effective model, and effective thinking level.
- A higher runtime seam should cover prompt continuation after branch resume, proving that a later resume sees the continuation rather than the old leaf target.
- The SDK seam should cover both linear resume success and non-linear fail-closed behavior through `SessionTopology`.
- Existing `SessionTree` tests remain low-level protection for tree indexing, active leaf restoration, branch navigation, and compaction-aware context reconstruction.
- Documentation validation should check updated markdown for accurate behavior, clear agent-facing language, and relative links.
- Focused validation should include the resume lifecycle tests, SDK resume tests, session tree tests, and architecture/public-boundary tests if public headers or dependency directions change.

## Out of Scope

- Adding branch selection CLI, REPL, or RPC commands.
- Making `model_change` or `thinking_level_change` alter provider/model resolution.
- Supporting full non-linear append semantics in SDK v1.
- Replacing the JSONL session format.
- Reworking provider configuration, project resources, command dispatch, tool execution, or prompt processing.
- Introducing a broad public session tree API for runtime callers.

## Further Notes

- This spec follows the architecture direction that data remains passive value state and capabilities cross physical seams through narrow interfaces.
- The desired module shape is a deeper session module: more resume behavior behind a smaller runtime-facing surface.
- The current branch already contains WIP implementation and tests; later issues should either refine that WIP or replace it without preserving known review findings.

## Implementation Summary

- Issue 01 implemented by `ef87f0b Deepen session resume open_session seam`.
- Issue 02 implemented by `c0d25d5 Persist branch continuation after resume`.
- Issue 03 implemented by `81f9f91 test sdk resume topology gating`.

Validation recorded across the implementation issues covers resume lifecycle tests, SDK resume tests, session tree tests, and the full `ctest --test-dir build --output-on-failure` suite.

# Issue 02: Convert AGENTS.md into the Root Router

Status: ready-for-agent

## Parent

.scratch/optimize-agents-md-root-router/PRD.md

## What to build

Rewrite the root agent instruction document into a concise English Root Router. It should front-load the predictable process agents must follow: Start Gate, Guardrails, compact Route, and pointers to detailed references. Keep always-needed safety and architecture rules in the root document, while relying on the detailed Route reference for branch-specific module detail.

The result should improve predictability, not merely reduce length: agents should start from the same process, classify the task, read only the needed references, and avoid overwriting user work.

## Acceptance criteria

- [x] The root document has a clear Start Gate that requires working-tree inspection before edits, task classification, minimal context selection, and active-plan discovery when architecture, public contracts, or pi parity are involved.
- [x] The no-overwrite rule for user changes remains prominent and checkable.
- [x] Architecture Guardrails remain root-level and preserve the current passive-value, seam, move-only event sink, serialization-locality, and forbidden-regression rules.
- [x] The compact Route section points agents to the detailed Route reference instead of carrying the full module matrix inline.
- [x] pi C++ parity direction remains visible and points to the roadmap and active implementation plans rather than duplicating the roadmap.
- [x] Issue-tracker, triage-label, and domain-doc skill pointers remain discoverable.
- [x] The document is written in clear English and optimized for agent execution.
- [x] Removed or condensed content has an explicit reference path to an authoritative replacement.

## Blocked by

- .scratch/optimize-agents-md-root-router/issues/01-extract-detailed-routing-reference.md

## Comments

- 2026-07-03 post-build review: `AGENTS.md` is the RootRouter. It preserves Start Gate, Guardrails, compact Route, pi parity direction, Worktree Discipline, Verify Slice, Handoff, and Agent References while detailed route and branch guidance live in external agent references.

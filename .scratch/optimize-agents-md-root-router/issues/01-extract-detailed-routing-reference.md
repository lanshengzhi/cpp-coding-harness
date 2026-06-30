# Issue 01: Prefactor detailed routing into an external Route reference

Status: ready-for-agent

## Parent

.scratch/optimize-agents-md-root-router/PRD.md

## What to build

Create a dedicated detailed Route reference for the repository's agent instructions, then move the full module routing matrix there without losing routing coverage. The root agent instruction document should be able to point to this reference, but this slice does not need to complete the full root-router rewrite.

This is the prefactor slice: make the later simplification easy by giving detailed routing a stable, co-located home first.

## Acceptance criteria

- [ ] A detailed Route reference exists in the agent documentation area and is reachable from the root agent instruction document.
- [ ] The detailed reference is written in clear English and optimized for agent execution.
- [ ] The detailed reference preserves every current task family, preferred seam, and routing note from the existing module route matrix.
- [ ] The root agent instruction document still works as the entry point after this slice; it may contain both the old routing content and the new pointer if needed for safety.
- [ ] Existing plan cross-references that depend on provider, tool, session, CLI, runtime, public-boundary, documentation, or pi-parity routing still land on equivalent concepts.
- [ ] Markdown links and headings render correctly.
- [ ] The change is documentation-only and does not alter C++ behavior, build configuration, CLI behavior, provider behavior, tool behavior, or session behavior.

## Blocked by

None - can start immediately

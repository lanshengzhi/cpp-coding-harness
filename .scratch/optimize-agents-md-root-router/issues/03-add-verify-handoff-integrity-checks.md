# Issue 03: Add Verify Slice, Handoff, and documentation integrity checks

Status: ready-for-agent

## Parent

.scratch/optimize-agents-md-root-router/PRD.md

## What to build

Complete the Root Router by adding explicit verification and handoff behavior. Agents should be able to choose the smallest meaningful Verify Slice for a task, escalate to architecture tests when public seams are touched, and report changed scope, validation, and skipped validation consistently.

This slice also performs the no-information-loss pass over the refactor: links, headings, route coverage, and historical cross-references should remain usable after the root document is shortened.

## Acceptance criteria

- [x] The root instructions include a Verify Slice section that maps documentation-only, implementation, public-boundary, and real-provider-sensitive changes to appropriate validation expectations.
- [x] Public header, include-boundary, provider/tool/session contract, or CMake public/private boundary changes are clearly tied to architecture-test validation.
- [x] Documentation-only changes are allowed to use markdown, link, heading, and no-information-loss checks without forcing a C++ build by default.
- [x] The root instructions include a Handoff section requiring changed scope, tests or checks run, and any intentionally skipped validation.
- [x] The verification pass checks that the affected agent-facing documentation is clear English and optimized for agent execution.
- [x] Detailed routing content remains reachable from the root document after the rewrite.
- [x] Existing plan references to the agent routing document remain resolvable to equivalent concepts.
- [x] The issue-tracker PRD and derived issue workflow remain marked with the ready-for-agent triage vocabulary.
- [x] The final state is documentation-only and does not alter C++ behavior, build configuration, CLI behavior, provider behavior, tool behavior, or session behavior.

## Blocked by

- .scratch/optimize-agents-md-root-router/issues/02-convert-agents-md-to-root-router.md

## Comments

- 2026-07-03 post-build review: Verify Slice and Handoff are present in the RootRouter; detailed route, historical section, branch guidance, issue-tracker, triage-label, and domain-doc references are reachable. Completion evidence is recorded in checkboxes while `Status: ready-for-agent` remains triage readiness.

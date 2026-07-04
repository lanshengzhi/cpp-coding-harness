# PRD: Optimize AGENTS.md as a Root Router

Status: ready-for-agent

## Problem Statement

`AGENTS.md` is the repo's **RootRouter**: the agent entry document that routes each task to start checks, guardrails, detailed references, validation, and handoff expectations. It currently carries useful guardrails and route coverage, but it still behaves too much like a compact handbook: the root document loads a long module route table and several operational details into every agent turn. That increases context load, makes the first-step process less predictable, and makes future updates harder because detailed routing, policy, and checklist content live on the same level.

The user wants `AGENTS.md` optimized so an agent reliably follows the same process every run: start safely, choose the right route or code seam, read only the necessary references, verify at the right slice, and hand off clearly. The optimization must preserve the current architecture guardrails, pi C++ parity direction, and local issue-tracker conventions while using clear English, agent-friendly documentation.

## Solution

Turn `AGENTS.md` into a concise root router with a small number of high-signal sections: Start Gate, Guardrails, Route, Verify Slice, and Handoff. Keep always-needed rules in the root document, and move branch-specific or detailed reference material behind explicit context pointers in the agent documentation area.

The full route matrix remains available as external reference, but the root document only carries enough routing information to send the agent to the right place. Completion criteria become checkable, especially for start-up, architecture-sensitive work, verification, and final responses.

## User Stories

1. As a repository maintainer, I want the root agent instructions to be shorter, so that every agent turn spends less context on rarely used routing detail.
2. As a repository maintainer, I want `AGENTS.md` to remain the entry routing seam, so that agents do not need to guess where repository instructions begin.
3. As an agent working in the repo, I want a clear Start Gate, so that I always check working tree state before editing.
4. As an agent working in the repo, I want the Start Gate to include task classification, so that I read the right reference material instead of the whole documentation set.
5. As an agent working in the repo, I want the Start Gate to define completion criteria, so that I know when initial context gathering is done.
6. As a user with uncommitted local changes, I want the root instructions to keep the no-overwrite rule prominent, so that my work is not accidentally replaced.
7. As a repository maintainer, I want architecture guardrails to stay in the root document, so that domain contract rules are always visible.
8. As a repository maintainer, I want forbidden regressions to have a single source of truth, so that future edits do not maintain duplicate ban lists.
9. As an agent changing public contracts, I want the root document to tell me when architecture tests are mandatory, so that public seams remain protected.
10. As an agent changing documentation only, I want the root document to avoid forcing C++ implementation context, so that documentation tasks stay focused.
11. As an agent working on unfamiliar code, I want a compact Route section, so that I can find the correct detailed reference without scanning a large table.
12. As an agent working on a familiar module, I want the root Route section to stay lightweight, so that I can proceed directly to the known seam.
13. As a repository maintainer, I want the detailed route matrix moved behind a pointer, so that branch-specific implementation detail is available without being always loaded.
14. As an agent working on pi parity, I want pi C++ direction and active-plan discovery preserved, so that parity work still follows the intended roadmap.
15. As an agent working on provider, tool, session, CLI, or runtime changes, I want detailed routing preserved in an external reference, so that no module entry point is lost.
16. As an agent working on skills or local project resources, I want the relevant agent-skill pointers preserved, so that skill behavior remains discoverable.
17. As a repository maintainer, I want long descriptions in route rows replaced by routing intent, so that the root document answers “where next?” rather than “how everything works.”
18. As an agent planning verification, I want a Verify Slice section, so that I choose the smallest meaningful test seam for the change.
19. As an agent modifying public headers or include boundaries, I want Verify Slice to escalate to architecture tests, so that boundary regressions are caught.
20. As an agent modifying documentation, I want Verify Slice to allow link, markdown, and no-information-loss checks, so that docs-only changes are validated without unnecessary builds.
21. As a repository maintainer, I want final-response expectations in a Handoff section, so that every agent reports changed scope, tests run, and skipped verification consistently.
22. As a future agent updating routing, I want co-located detailed routing reference, so that module route changes have an obvious home.
23. As a future agent pruning stale content, I want root instructions separated from reference, so that stale reference can be updated without weakening guardrails.
24. As a reviewer, I want the optimized structure to preserve cross-references from existing plans, so that historical plans remain navigable.
25. As a reviewer, I want the optimized structure to use clear English wording, so that agents can execute the instructions without translation overhead.
26. As a maintainer, I want no C++ behavior change from this work, so that the optimization is safe and reviewable as documentation refactoring.
27. As an agent following local issue-tracker conventions, I want the PRD and any derived implementation issues to use the ready-for-agent status, so that the work can be picked up without extra triage.
28. As a user asking for AGENTS optimization, I want the result to improve predictability rather than merely shorten text, so that agents take the same process even when outputs differ.

## Implementation Decisions

- Treat the root agent instruction document as the repository's RootRouter. Its job is to route, guard, and define completion criteria, not to duplicate every detailed module reference.
- Write the optimized root instructions and new agent documentation in clear English, optimized for agent execution rather than human narrative.
- Use progressive disclosure: keep always-needed process and guardrails in the root document; move branch-specific route details into a dedicated agent reference document.
- Introduce stable leading words for the root process: Start Gate, Guardrails, Route, Verify Slice, and Handoff.
- Start Gate should require working-tree inspection, task classification, minimal context selection, and active-plan discovery only when the task affects architecture, public contracts, or pi parity.
- Guardrails should remain visible in the root document because they apply across every task branch.
- The detailed route matrix should remain complete, but it should be external reference reached from the compact Route section.
- The compact Route section should describe task families and where to continue, not implementation internals.
- Verify Slice should connect task type to the highest practical validation seam: architecture tests for public boundary changes, focused C++ tests for implementation changes, and documentation/link/no-information-loss checks for docs-only changes.
- Handoff should require a concise final report naming changed scope, validation performed, and any validation intentionally skipped.
- Existing local issue-tracker and triage vocabulary should stay as pointers rather than being duplicated in the root document.
- Existing pi C++ parity direction should stay visible, with detailed roadmap content left in planning documents.
- Do not change C++ contracts, CLI behavior, session schema, provider behavior, tool behavior, or build configuration as part of this PRD.
- Do not create a new runtime seam; this is a documentation refactor around the existing agent instruction seam.

## Testing Decisions

- A good test checks external documentation behavior: after reading the root instructions, an agent can identify the Start Gate, apply Guardrails, choose a Route, select a Verify Slice, and produce a Handoff without needing unrelated detail.
- Use the highest seam possible: review the root routing document as one behavior surface, then review the detailed route reference only for no-information-loss.
- Validate that the root document still instructs agents not to overwrite user changes before editing.
- Validate that every architecture guardrail from the current root instructions remains present or has an explicit root-level equivalent.
- Validate that every removed detailed route row is still available through the external route reference.
- Validate that existing references from plans to the agent routing document still land on equivalent concepts after the refactor.
- Validate that pi C++ parity guidance still points agents toward the roadmap and active implementation plans.
- Validate that the issue-tracker, triage-label, and domain-doc skill pointers remain discoverable.
- Validate markdown rendering and relative links for any new or moved reference documents.
- Validate that docs-only changes do not require a C++ build unless the implementation changes unexpectedly touch code, public headers, or build files.
- Prior art: the existing completed cleanup plan for progressive disclosure already established that build commands and stale implementation details should be referenced rather than duplicated. This PRD extends that direction by splitting detailed routing reference out of the root instructions.

## Out of Scope

- Changing C++ implementation behavior.
- Changing provider, tool, session, CLI, runtime, or SDK contracts.
- Changing the build system or test executable layout.
- Translating unrelated repository documentation outside the affected agent-instruction routing surface.
- Rewriting the README or active planning documents except for link maintenance if absolutely required.
- Introducing a custom documentation parser or new CI job.
- Creating implementation issues beyond this PRD unless the maintainer asks for task breakdown.
- Modifying local issue-tracker conventions or triage vocabulary.

## Further Notes

- No ADR files were found for this area during exploration.
- The domain glossary reserves “Seam” for module/interface boundaries; this PRD uses “RootRouter” for `AGENTS.md`.
- The current working tree was clean before publishing this PRD.
- This PRD intentionally synthesizes from the current conversation and repo state without interviewing the user further.

## Post-build Review

- 2026-07-03: Reviewed as an already-applied documentation refactor. `Status: ready-for-agent` remains a triage-readiness state; completion evidence belongs in acceptance checkboxes and review comments.
- Row-level route preservation is the review standard: every pre-router route row must have a corresponding row in `docs/agents/module-routing.md`.
- Detailed branch, merge, publish, PR, and cleanup guidance belongs in `docs/agents/worktree-discipline.md` rather than the RootRouter.
- Tool, provider, session, workspace, and documentation-specific change rules belong in the relevant rows of `docs/agents/module-routing.md`.

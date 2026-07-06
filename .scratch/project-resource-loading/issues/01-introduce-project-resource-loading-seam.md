# Introduce Project Resource Loading Seam

Status: ready-for-agent

## Parent

.scratch/project-resource-loading/PRD.md

## What to build

Create the central project resource-loading seam described by the PRD. It should
accept session-start resource inputs, apply existing project trust/resource
policy, invoke resource adapters for the implemented kinds, and return a loaded
resource bundle plus structured diagnostics.

This slice should be behavior-preserving and directly testable without migrating
CLI or SDK call sites yet. Use skills and prompt templates as the first adapters.
Unsupported future markers should still appear as unsupported decisions and must
not be parsed.

## Acceptance criteria

- [ ] A single project resource-loading entry point can produce loaded skills, loaded prompt templates, a load plan, trust information, and diagnostics.
- [ ] The seam does not parse project-local skill or prompt template contents until the load plan allows that resource kind.
- [ ] Skill and prompt-template parsing remain delegated to their existing loaders.
- [ ] Project skills and project prompts are loaded only when detected, supported, enabled, and trusted.
- [ ] Unsupported project markers are represented as skipped/unsupported decisions without triggering trust resolution when no supported resource could load.
- [ ] Explicit prompt template paths can be represented as user-provided loading inputs distinct from project marker discovery.
- [ ] Duplicate project resources are diagnosed through the shared seam rather than by call-site-specific logic.
- [ ] Focused tests cover trusted load, untrusted skip, disabled skip, unsupported markers, malformed adapter input, and duplicate decisions.
- [ ] Existing project trust/resource, skill loader, prompt template loader, and prompt expansion tests still pass.

## Blocked by

None - can start immediately.

## Validation

Run the focused coding-agent test slice for project resources, project trust,
skill loading, prompt template loading, and prompt expansion. If public headers
or CMake dependency boundaries change, also run the architecture tests.

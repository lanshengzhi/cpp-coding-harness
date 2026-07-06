# Introduce Project Resource Loading Seam

Status: implemented

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

- [x] A single project resource-loading entry point can produce loaded skills, loaded prompt templates, a load plan, trust information, and diagnostics.
- [x] The seam does not parse project-local skill or prompt template contents until the load plan allows that resource kind.
- [x] Skill and prompt-template parsing remain delegated to their existing loaders.
- [x] Project skills and project prompts are loaded only when detected, supported, enabled, and trusted.
- [x] Unsupported project markers are represented as skipped/unsupported decisions without triggering trust resolution when no supported resource could load.
- [x] Explicit prompt template paths can be represented as user-provided loading inputs distinct from project marker discovery.
- [x] Duplicate project resources are diagnosed through the shared seam rather than by call-site-specific logic.
- [x] Focused tests cover trusted load, untrusted skip, disabled skip, unsupported markers, malformed adapter input, and duplicate decisions.
- [x] Existing project trust/resource, skill loader, prompt template loader, and prompt expansion tests still pass.

## Blocked by

None - can start immediately.

## Validation

Run the focused coding-agent test slice for project resources, project trust,
skill loading, prompt template loading, and prompt expansion. If public headers
or CMake dependency boundaries change, also run the architecture tests.

Completed validation:

- `./build/cpp_harness_tests "[coding_agent][project-resource-loader]"`
- `./build/cpp_harness_tests "[coding_agent][project-resources]"`
- `./build/cpp_harness_tests "[coding_agent][project-trust]"`
- `./build/cpp_harness_tests "[coding_agent][skill]"`
- `./build/cpp_harness_tests "[coding_agent][prompt][loader]"`
- `./build/cpp_harness_tests "[coding_agent][prompt][expand]"`
- `./build/cpp_harness_tests "[architecture]"`
- `ctest --test-dir build --output-on-failure`

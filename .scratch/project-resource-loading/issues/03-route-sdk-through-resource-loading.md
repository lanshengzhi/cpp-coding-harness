# Route SDK Project Resources Through Shared Loading

Category: enhancement
Status: implemented

## Parent

.scratch/project-resource-loading/spec.md

## What to build

Migrate public SDK session creation to use the same project resource-loading
seam as the CLI. Preserve the public SDK options and behavior while removing the
separate SDK-only trust/resource loading path.

Host-provided skills and prompt templates must remain first-class startup
resources. They take precedence over project-discovered duplicates, and all
warnings must be returned as SDK diagnostics rather than printed.

## Acceptance criteria

- [x] Public SDK session creation uses the shared project resource-loading seam when project resource loading is enabled.
- [x] Host-provided skills are present even when project resource loading is disabled or untrusted.
- [x] Host-provided prompt templates are present even when project resource loading is disabled or untrusted.
- [x] Project-discovered skill duplicates are skipped when a host skill with the same name exists, with a structured diagnostic.
- [x] Project-discovered prompt template duplicates are skipped when a host template with the same name exists, with a structured diagnostic.
- [x] SDK diagnostics include trust, resource-decision, adapter, and duplicate warnings as values.
- [x] SDK creation does not write resource diagnostics to stdout or stderr.
- [x] SDK tests cover trusted project load, untrusted skip, host precedence, malformed resource diagnostics, and prompt/skill invocation after loading.
- [x] Existing SDK session tests continue to pass.

## Blocked by

.scratch/project-resource-loading/issues/01-introduce-project-resource-loading-seam.md

## Validation

Run the SDK session tests plus the focused resource-loading tests. If this slice
touches public SDK headers, run the architecture tests.

Completed validation:

- `cmake --build build --target cpp_harness_tests`
- `./build/cpp_harness_tests "[sdk]"`
- `./build/cpp_harness_tests "[coding_agent][project-resource-loader]"`
- `./build/cpp_harness_tests "[cli][project-resources]"`
- `git diff --check`
- `ctest --test-dir build --output-on-failure`

Code review:

- Standards review found no hard documented-standard violations.
- Spec review found an SDK trust-store compatibility issue in the initial
  implementation; the final implementation preserves the SDK workspace-local
  trust store while still routing through the shared loader.

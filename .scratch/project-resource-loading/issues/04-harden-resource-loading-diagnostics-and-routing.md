# Harden Resource Loading Diagnostics And Routing

Category: enhancement
Status: implemented

## Parent

.scratch/project-resource-loading/spec.md

## What to build

Finish the refactor by removing obsolete call-site-specific resource loading
logic, hardening diagnostic taxonomy and documentation, and updating agent
routing so future resource work starts at the shared resource-loading seam.

This slice should make the module ready for later project settings, system
prompt files, extensions, packages, and global/config-driven resource dirs
without implementing those resource kinds.

## Acceptance criteria

- [x] Trust diagnostics, load-plan diagnostics, adapter diagnostics, and duplicate/collision diagnostics use stable categories and are not emitted twice.
- [x] CLI and SDK diagnostic formatting are thin presentation layers over the same structured diagnostics.
- [x] Future project resource markers remain represented as unsupported decisions in tests.
- [x] Obsolete direct project resource loading paths are removed or reduced to compatibility shims.
- [x] Route documentation points project trust/resource, skills/resource loading, prompt-template loading, CLI/runtime, and SDK resource work to the shared resource-loading seam.
- [x] README or plan notes are updated only if user-visible behavior or architecture ownership changed.
- [x] Tests protect that JSON/RPC stdout stays protocol-clean for resource diagnostics.
- [x] Architecture tests pass if public headers, include surfaces, or CMake boundaries changed.

## Blocked by

.scratch/project-resource-loading/issues/02-route-cli-through-resource-loading.md
.scratch/project-resource-loading/issues/03-route-sdk-through-resource-loading.md

## Validation

Run the focused resource-loading, CLI smoke, SDK session, and architecture test
slices. No live provider validation is required.

Performed:

- `cmake --build build`
- `./build/cpp_harness_tests "[coding_agent][project-resource-loader]"`
- `./build/cpp_harness_tests "[cli][project-resources]"`
- `./build/cpp_harness_tests "[cli][json][project-resources]"`
- `./build/cpp_harness_tests "[cli][rpc][project-resources]"`
- `./build/cpp_harness_tests "[sdk][project-resources]"`
- `./build/cpp_harness_tests "[cli][project-trust]"`
- `./build/cpp_harness_tests "[architecture]"`
- `./build/cpp_harness_tests`

Skipped:

- Live provider validation; not required for this issue.

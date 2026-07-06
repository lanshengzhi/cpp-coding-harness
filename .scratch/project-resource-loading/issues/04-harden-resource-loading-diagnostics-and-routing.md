# Harden Resource Loading Diagnostics And Routing

Status: ready-for-agent

## Parent

.scratch/project-resource-loading/PRD.md

## What to build

Finish the refactor by removing obsolete call-site-specific resource loading
logic, hardening diagnostic taxonomy and documentation, and updating agent
routing so future resource work starts at the shared resource-loading seam.

This slice should make the module ready for later project settings, system
prompt files, extensions, packages, and global/config-driven resource dirs
without implementing those resource kinds.

## Acceptance criteria

- [ ] Trust diagnostics, load-plan diagnostics, adapter diagnostics, and duplicate/collision diagnostics use stable categories and are not emitted twice.
- [ ] CLI and SDK diagnostic formatting are thin presentation layers over the same structured diagnostics.
- [ ] Future project resource markers remain represented as unsupported decisions in tests.
- [ ] Obsolete direct project resource loading paths are removed or reduced to compatibility shims.
- [ ] Route documentation points project trust/resource, skills/resource loading, prompt-template loading, CLI/runtime, and SDK resource work to the shared resource-loading seam.
- [ ] README or plan notes are updated only if user-visible behavior or architecture ownership changed.
- [ ] Tests protect that JSON/RPC stdout stays protocol-clean for resource diagnostics.
- [ ] Architecture tests pass if public headers, include surfaces, or CMake boundaries changed.

## Blocked by

.scratch/project-resource-loading/issues/02-route-cli-through-resource-loading.md
.scratch/project-resource-loading/issues/03-route-sdk-through-resource-loading.md

## Validation

Run the focused resource-loading, CLI smoke, SDK session, and architecture test
slices. No live provider validation is required.

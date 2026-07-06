# Route SDK Project Resources Through Shared Loading

Status: ready-for-agent

## Parent

.scratch/project-resource-loading/PRD.md

## What to build

Migrate public SDK session creation to use the same project resource-loading
seam as the CLI. Preserve the public SDK options and behavior while removing the
separate SDK-only trust/resource loading path.

Host-provided skills and prompt templates must remain first-class startup
resources. They take precedence over project-discovered duplicates, and all
warnings must be returned as SDK diagnostics rather than printed.

## Acceptance criteria

- [ ] Public SDK session creation uses the shared project resource-loading seam when project resource loading is enabled.
- [ ] Host-provided skills are present even when project resource loading is disabled or untrusted.
- [ ] Host-provided prompt templates are present even when project resource loading is disabled or untrusted.
- [ ] Project-discovered skill duplicates are skipped when a host skill with the same name exists, with a structured diagnostic.
- [ ] Project-discovered prompt template duplicates are skipped when a host template with the same name exists, with a structured diagnostic.
- [ ] SDK diagnostics include trust, resource-decision, adapter, and duplicate warnings as values.
- [ ] SDK creation does not write resource diagnostics to stdout or stderr.
- [ ] SDK tests cover trusted project load, untrusted skip, host precedence, malformed resource diagnostics, and prompt/skill invocation after loading.
- [ ] Existing SDK session tests continue to pass.

## Blocked by

.scratch/project-resource-loading/issues/01-introduce-project-resource-loading-seam.md

## Validation

Run the SDK session tests plus the focused resource-loading tests. If this slice
touches public SDK headers, run the architecture tests.

# Route CLI Session Creation Through Resource Loading

Status: ready-for-agent

## Parent

.scratch/project-resource-loading/PRD.md

## What to build

Migrate CLI/RPC session creation to use the shared project resource-loading seam
instead of building project skill directories, project prompt directories, trust
decisions, and resource diagnostics inline.

The visible CLI behavior should remain unchanged: project skills and prompt
templates load only when allowed, explicit prompt-template paths still work,
diagnostics are shown on stderr, and JSON/RPC stdout remains protocol-clean.

## Acceptance criteria

- [ ] CLI session creation obtains loaded skills and prompt templates from the shared resource-loading seam.
- [ ] Runtime service construction receives already-loaded resources and no longer owns project directory scanning or direct resource diagnostic printing.
- [ ] `--approve`, `--no-approve`, `--no-skills`, `--prompt-template`, and `--no-prompt-templates` preserve their current behavior.
- [ ] Text-mode resource diagnostics are still visible on stderr.
- [ ] JSON mode emits only valid JSONL records on stdout when project resources are malformed, skipped, or duplicated.
- [ ] RPC mode command responses remain protocol-clean when project resources are malformed, skipped, or duplicated.
- [ ] CLI smoke or integration tests cover trusted project skill loading, trusted project prompt loading, untrusted skip, disabled skip, explicit prompt template path loading, and malformed resource diagnostics.
- [ ] Existing CLI smoke tests and runtime session tests continue to pass.

## Blocked by

.scratch/project-resource-loading/issues/01-introduce-project-resource-loading-seam.md

## Validation

Run the CLI smoke tests plus the focused coding-agent runtime/session tests. Use
the fake provider only; no live provider validation is required.

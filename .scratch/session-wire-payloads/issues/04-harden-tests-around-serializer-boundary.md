# Harden tests around the serializer boundary

Status: ready-for-agent

## Parent

.scratch/session-wire-payloads/PRD.md

## What to build

Finish the session-wire cleanup by making tests express the intended boundary.
Known-entry tests should assert typed loaded values and user-visible session
behavior. Tests that care about exact JSON field names should be clearly scoped
to the serializer or wire-format boundary. This issue should also remove or
rename any stale test helpers or comments that imply generic payload access is
the primary domain interface.

This issue is the final integration pass for the PRD. It should verify that the
session module no longer relies on raw wire payloads in known-entry domain
logic, while still preserving diagnostics and unknown-entry compatibility.

## Acceptance criteria

- [ ] Store tests for known session entries assert typed entry values instead of generic JSON payload shape.
- [ ] Wire-format tests still explicitly protect the current JSONL field names where compatibility matters.
- [ ] Session tree tests assert behavior through context reconstruction, branch navigation, and leaf restoration rather than raw payload fields.
- [ ] Unknown-entry preservation remains covered.
- [ ] Stale comments or helper names that describe the old payload-driven seam are updated where touched.
- [ ] A search for known-entry wire-field lookups in session domain logic shows none outside the serializer or intentional wire-format tests.
- [ ] Architecture tests pass if public session headers or dependency boundaries changed.
- [ ] Focused session store, session tree, resume lifecycle, and relevant SDK resume tests pass.

## Blocked by

- .scratch/session-wire-payloads/issues/02-move-session-tree-context-to-typed-values.md
- .scratch/session-wire-payloads/issues/03-move-leaf-resume-and-append-continuation-off-wire-payloads.md

# Introduce typed session entry values

Status: ready-for-agent

## Parent

.scratch/session-wire-payloads/PRD.md

## What to build

Make the session load seam produce typed passive values for every known
non-message session entry kind. A loaded known entry should carry its domain
meaning directly, instead of requiring callers to inspect generic JSON payloads.

The slice should preserve JSONL compatibility, unknown-entry preservation,
redaction behavior, existing message loading, and existing metadata loading.
Raw payload or raw-line state may remain available for diagnostics and unknown
entries, but known-entry behavior should be accessible through typed values.

## Acceptance criteria

- [ ] Loaded known entries expose typed passive values for model changes, thinking-level changes, active-tools changes, custom entries, custom-message entries, labels, compactions, branch summaries, session info, and leaf markers.
- [ ] Message entries continue to expose parsed provider-neutral message values.
- [ ] Unknown entries remain preserved as unknown diagnostics or unknown lines without gaining false typed semantics.
- [ ] JSONL wire field names and Glaze DTO details stay owned by the serializer boundary.
- [ ] Existing sessions written by the repository still load successfully.
- [ ] Store tests assert typed entry values for each known entry kind.
- [ ] Critical wire-format compatibility checks remain covered at the serializer/store boundary.
- [ ] Secret redaction behavior remains unchanged.
- [ ] Focused session store tests pass.

## Blocked by

None - can start immediately

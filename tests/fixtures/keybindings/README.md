# Keybinding compatibility fixtures

These fixtures record the supported configuration shape at pi parity baseline
`83114817` (re-pinned from `864b35c` by the pi-coding-agent phase audit, ADR 0036; the pinned
`packages/tui/src/keybindings.ts` and `packages/coding-agent/src/core/keybindings.ts` tables are
byte-identical between the two commits). They were derived from:

- `pi:packages/tui/src/keybindings.ts`
- `pi:packages/coding-agent/src/core/keybindings.ts`
- `pi:packages/coding-agent/docs/keybindings.md`

The fixture intentionally mixes scalar and array values with invalid and
unassembled IDs so discovery and diagnostic behavior remain explicit. The
harness reads compatible files only from its own Agent Config Directory; the
fixture does not authorize implicit discovery from pi state directories.

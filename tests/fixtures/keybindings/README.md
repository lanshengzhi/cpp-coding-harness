# Keybinding compatibility fixtures

These fixtures record the supported configuration shape at pi parity baseline
`864b35c`. They were derived from:

- `pi:packages/tui/src/keybindings.ts`
- `pi:packages/coding-agent/src/core/keybindings.ts`
- `pi:packages/coding-agent/docs/keybindings.md`

The fixture intentionally mixes scalar and array values with invalid and
unassembled IDs so discovery and diagnostic behavior remain explicit. The
harness reads compatible files only from its own Agent Config Directory; the
fixture does not authorize implicit discovery from pi state directories.

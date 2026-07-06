# Move SessionTree context reconstruction to typed values

Status: implemented

## Parent

.scratch/session-wire-payloads/PRD.md

## What to build

Update session tree context reconstruction so it consumes typed session entry
values rather than generic JSON payload fields. The visible behavior should stay
the same: model and thinking-level context should be extracted from the active
path, compaction should shape resumed context, branch summaries should become
branch summary messages, and custom message entries should become custom
messages.

This is a domain refactor, not a session behavior change. Tree logic should no
longer know wire field names such as model IDs, thinking levels, compaction
fields, branch summary fields, or custom message fields.

## Acceptance criteria

- [x] Session tree context reconstruction reads model change values from typed entries.
- [x] Session tree context reconstruction reads thinking-level values from typed entries.
- [x] Compaction context reconstruction reads summary, first-kept entry ID, and token metadata from typed entries.
- [x] Branch summary entries are converted to branch summary messages from typed values.
- [x] Custom message entries are converted to custom messages from typed values.
- [x] Generic JSON payload lookup is not used for known-entry context reconstruction.
- [x] Existing linear, branched, and compacted resume behavior is unchanged.
- [x] Session tree tests cover the same user-visible behavior after the refactor.
- [x] Focused session tree and resume lifecycle tests pass.

## Blocked by

None - issue 01 is implemented.

## Comments

- Implemented by moving `SessionTree` context reconstruction to typed
  `SessionEntryValue` alternatives for model, thinking-level, compaction,
  branch summary, and custom message entries.
- Kept wire field names and raw payload extraction inside `EntrySerializer`;
  `SessionTree` tests now cover behavior with misleading known-entry payloads.
- Verified with focused session store/tree, runtime resume, SDK topology, and
  architecture test slices.

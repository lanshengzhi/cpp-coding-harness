# Move leaf resume and append continuation off wire payloads

Status: ready-for-agent

## Parent

.scratch/session-wire-payloads/PRD.md

## What to build

Update leaf restoration, invalid leaf fallback, and branch-continuation append
behavior so leaf target information comes from typed leaf entry values rather
than generic JSON payload fields. Resuming from a branch and then continuing the
conversation should still make newly appended messages the next resume point.

This slice protects the branch-resume persistence behavior introduced by the
session resume module work while removing the remaining wire-field dependency
from the leaf path.

## Acceptance criteria

- [ ] Valid persisted leaf markers restore the active branch using typed leaf values.
- [ ] Invalid persisted leaf markers fall back to the last valid navigable entry using typed entry state.
- [ ] Opening an existing session determines the active append parent without generic JSON target lookup.
- [ ] Appending after branch resume still persists a new leaf marker for the appended message.
- [ ] A later resume includes continuation messages instead of snapping back to the old leaf.
- [ ] Generic JSON payload lookup is not used for known leaf restoration or branch-continuation append behavior.
- [ ] Existing SDK resume topology behavior is unchanged.
- [ ] Focused resume lifecycle, session tree, and SDK resume topology tests pass when relevant.

## Blocked by

None - issue 01 is implemented.

# Issue 1221: terminal stop evidence clarification

## Scope

This note clarifies the modifyOtherKeys finding in
[1182-tui-reconciliation.md](1182-tui-reconciliation.md) and the B13 ticket. It
makes no capability promotion or delivery-receipt claim.

## Frozen observation

The registered `audit-1050/program-status-detection.json` capture records a
modifyOtherKeys disable at stop. In every captured scenario,
`program-status-child.mts` sends a device-attributes reply before `stop()`;
there is no Kitty flags reply. The frozen pi-v1.3.0 source at revision
`71023aec1546d6d677a65669c5665c0927ce0dd0`,
`packages/tui/src/terminal.ts`, enables modifyOtherKeys on that reply and its
`disableModifyOtherKeys()` returns without writing if the mode is inactive.
Consequently the captured disable is conditional on the earlier negotiation;
the capture does not establish an unconditional restore or a Kitty-negotiated
stop sequence. The contrary Kitty inference and unconditional-restore remedy
in the #1182 finding do not follow from that capture.

The C++ fallback stop now pops Kitty before disabling modifyOtherKeys. The
program-status PTY comparison waits for the fallback, then compares the entire
concatenated recorded stop stream. A separate Kitty-negotiated PTY control
checks that no inactive fallback is disabled. The latter is supported by the
frozen source, not by a newly registered frozen runtime capture. PTY output is
not real terminal-emulator evidence.

## Validation

The owning TUI shard built and the fallback, captured program-status stop, and
Kitty-control selections passed (3 cases). The incremental full build passed.
The full CTest run selected 4,515 cases and failed four, with 11 skipped: two
consolidation checks lacked the lane identity in their environment, one
concurrent durable-edit comparison failed, and a Herdr PTY evidence case required
`TERM_PROGRAM=herdr`. A subsequent selection with the owning lane environment
passed both consolidation checks, the durable-edit case, and both program-status
cases (5 cases). This rerun does not turn the full suite into a pass; Herdr and
the skipped cases remain unverified in this environment. The independent
standards and specification reviews required by the implementation workflow
remain outstanding.

## Capture preflight

The local `../pi` checkout is clean at the required full revision and its
`node_modules/.bin/tsx` is available. A preflight invocation of
`fixtures/pi-tui/capture/capture-named-tui.mts` with `PI_BASELINE=pi-v1.3.0`
failed during transformation, before any output was written: the script has
an extra `] as const;` at line 915. This defect predates the #1221 commits.
Even after repairing that syntax, its separate TUI registry currently contains
only `pi-v1.0.4`, so the requested baseline is not selectable there. The
script also writes the entire selected bundle and updates its registry; it is
not a safe targeted recapture of the three B13 families. A dedicated v1.3.0
capture and registration, with review of affected receipt inputs, is still
needed. The existing `audit-1050` observations remain registered evidence,
but they do not cover the complete B13 comparison.

The local TUI test shard and the incremental full build passed, and the three
focused stop tests passed on this checkout. The subsequent unfiltered CTest run
selected 4,515 cases: two failed and 11 skipped (exit 8). The quarantined
projection cost test measured 2.279 ms of processor time against its 2 ms
bound; the Herdr PTY test required `TERM_PROGRAM=herdr`, which was unset.
These checks do not establish emulator behavior or whole-row verification.

## Remaining boundary

Ticket #1221 remains open: this clarification and the focused tests do not
compare all 110 rows with normal and distinct discriminating cases, register
new frozen captures or delivery receipts, reconcile the ledger, or establish
real-emulator behavior. Existing row status is unchanged. The shifted-symbol
and fragmented-reply tests already present are not a substitute for auditing
each row and its required discriminating case. Rows routed to other tickets
still require their respective owners. Any promotions must use the delivery
receipt contract and the #1229 discriminating-case requirement.

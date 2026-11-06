# #1240 masked-context discriminator probe, 2026-11-06

The registered `audit-1057/context-cancellation.json` scenario
`withoutAbortSignal removes cancellation while preserving typed values` supplies
its typed value and expected masked state. The new CTest case `a masked context
retains its value when its parent is cancelled later` creates the mask before
cancelling the parent, then compares that state to the frozen masked state. It
rejects a pass-through that follows later cancellation and a mask that discards
the typed value. The original comparison masks an already-cancelled parent;
the later-cancellation ordering is an additional C++ probe, not a separately
recorded upstream observation.

The focused case passed. After committing its source at `95e1faa`, the chord
selection passed 90/90 and the `issue1184` selection passed 10/10. Five
receipts pin `tests/chord/ChordContextTest.cpp`; their input digests and
validation records were refreshed from those runs. Consolidation build/check
and `receipt_boundary.py` passed with the claim still bounded.

Attempting to clear the boundary in the existing #1178 receipt failed the
consolidation stale-field check: promotion sets `implementation_ticket` to the
closed #1178, producing a new contradiction not in the baseline. The attempted
promotion was reverted. A continuation must establish an appropriate open
receipt or other valid mapping, and confirm that this additional ordering is
sufficiently supported by the frozen scenario before promotion. The other 14
rows still need independent discriminators and receipt reconciliation. No row
is promoted by this probe.

# TUI route batches B1–B17

Dated record read at `lanshengzhi/cpp-coding-harness@d8182efc9` (2026-11-06). It records the pi v1.3.0 TUI row partition (#1209–#1225), the receipt discipline that governs it and the measured ledger effect. It does not say what any batch's code does today. Authority: [ADR 0075](../../adr/0075-align-selected-owner-packages-and-linux-capabilities-with-pi-v1-3-0.md); delivery contract: [1058 delivery reconciliation](../../research/pi-v1.3.0/1058-delivery-reconciliation.md); row partition and route-batch registry: issue #1182 and `fixtures/pi-v1.3.0/consolidation-1058/`.

## Partition
- The owner decisions of 2026-11-04 filed the TUI rows as batches B1–B17 in issues #1209–#1225 ([decisions](decisions-2026-11-04.md)). One ticket per lane, one lane at a time ([ops-lanes](ops-lanes.md)).
- #1221 (B13) states the batch obligation in its own acceptance criteria: each of its 110 `cch_tui:` rows is either receipted whole-row through the delivery contract or left unverified with a named blocker.

## Batch state as measured at `d8182efc9` (2026-11-06)
`Rows` is the number of consolidated ledger rows naming that batch's receipt at that commit (`docs/research/pi-v1.3.0/1058-consolidated-capability-ledger.json`), after the #1229 bounded-claim reverts; it is not the receipt's claim count.

| Batch | Ticket | Issue state | Rows |
| --- | --- | --- | --- |
| B1 root TUI lifecycle | #1209 | closed 2026-11-05 | 29 |
| B2 regular renderer | #1210 | closed 2026-11-05 | 6 |
| B3 alt-screen viewport | #1211 | closed 2026-11-05 | 8 |
| B4 transcript selection | #1212 | closed 2026-11-05 | 16 |
| B5 alt-screen overlay/wheel/mouse | #1213 | closed 2026-11-05 | 18 |
| B6 kitty image placement/eviction | #1214 | open | 0 |
| B7 Editor layout/history/undo/kill-yank | #1215 | closed 2026-11-05 | 80 |
| B8 Input editing/paste/submit | #1216 | closed 2026-11-05 | 30 |
| B9 Markdown/tables/math | #1217 | closed 2026-11-05 | 51 |
| B10 Color/palettes/capability | #1218 | closed 2026-11-05 | 49 |
| B11 SelectList/SettingsList | #1219 | closed 2026-11-05 | 84 |
| B12 Layout algebra/scroll containers | #1220 | closed 2026-11-06 | 104 |
| B13 terminal IO | #1221 | open | 16 of 110 |
| B14 images | #1222 | open | 0 |
| B15 autocomplete | #1223 | open | 0 |
| B16 loader/mouse/residual | #1224 | open | 0 |
| B17 application entries/emulator | #1225 | open | 0 |

## The #1229 receipt discipline (2026-11-06)
- The retro review of `6ca7b49..e6e1de4a8` (issue #1229) found two evidence-integrity defects in the first TUI receipts: thirty #1209 rows cited `ScreenStateGoldenTest.cpp`, which replays frozen frames in a static virtual terminal and never exercises `preserveScreen`, alternate-screen exit, `renderNow()` or mouse handling; and 182 claims across the #1201–#1213 receipts named a `discriminatingCase` identical to one of the same row's `comparisons[].test` entries.
- Disposition: `consolidate_audits.py` gained a bounded-claim channel — a claim with a non-empty `remainingBoundary` is validated exactly like any other but does not promote its row, keeping the row's source-audit status under the fail-closed rule. The 182 claims were bounded in place; sixteen #1209 rows were withdrawn by #1236 and the remaining fourteen through `docs/research/pi-v1.3.0/1229-withdrawn-1209-root-tui-lifecycle-claims.json`.
- The rule is now binding on later batches: #1221's comment requires every row's `discriminatingCase` to cite a real negative, control or boundary case, never a happy-path test.
- 2026-11-06, `d8182efc9`: `tui_route_batches.delivered_rows()` counted bounded claims as delivered, which removed a bounded row from its batch's row set and nulled the batch's implementation obligation while the ledger kept it `partial`. The generator now skips bounded claims; the #1182 index pin and generator pin carry the `revalidation1221BoundedRows` rerun (13/13).

## B13 (#1221) slice record
- Slice A, delivered at `cba033bd0` (implementation `ae3f90422`): nine key rows — `cch_tui:Key`, `KeyEventType`, `KeyId`, `decodeKittyPrintable`, `isKeyRelease`, `isKeyRepeat`, `keys:shifted-symbol-matching`, `matchesKey`, `parseKey` — on a new registered `audit-1221-key-surface` family plus the existing `audit-1221` identities and an `audit-1050` shifted-symbol observation. Record: [1221-terminal-io-delivery](../../research/pi-v1.3.0/1221-terminal-io-delivery.md).
- Slice B, delivered at `8f5dc4263` (capture `a4d33beaf`): six `StdinBuffer` rows plus `cch_tui:stdin-buffer:fragmented-terminal-replies`, on three registered process-stream `audit-1221-stdin-framing` scenarios in which a fragmented OSC reply leaking into typed input fails. `cch_tui:StdinBuffer.flush` stays a bounded claim (C++ `flush()` emits where frozen pi only returns the fragment).
- At `d8182efc9`, 16 of the batch's 110 rows are delivery-backed verified. The remaining rows keep named blockers: terminal write/lifecycle plus real-emulator evidence, forwarded input, the mutable keybinding-manager semantics, and `isKittyProtocolActive`/`setKittyProtocolActive` (pi module-global state against C++ per-decoder state). Planning detail: `.lane-notes/b13-delivery-plan.md` (untracked working file).

## Ledger effect
- At `d8182efc9` the consolidated ledger has 1464 rows: **609 delivery-backed verified**, 529 unverified, 298 upstream-captured, 28 not-applicable; `cch_tui` carries 498 of the 925 verified rows. The same day's #1229 gate recorded 195 verified ([evidence history](evidence-and-delivery-history.md)), so the B7–B12 and B13 slices A/B receipts added 414 verified rows.

Related: [evidence and delivery history](evidence-and-delivery-history.md), [goals](goals.md), [ops-lanes](ops-lanes.md), [index](index.md).

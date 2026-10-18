# TUI v1.0.4 close-out evidence

- **Baseline**: `pi-v1.0.4` at `7c10bd4337495ee613f2224843ecdf349b80d1df`
- **Authority**: [ADR 0067](../adr/0067-align-the-linux-tui-toolkit-and-native-tui-with-pi-v1-0-4.md), spec [#946](https://github.com/lanshengzhi/cpp-coding-harness/issues/946), close-out ticket [#1025](https://github.com/lanshengzhi/cpp-coding-harness/issues/1025)
- **Delivery branch**: `pi946-closeout` (local assembly; the owner merges it into the integration branch — this ticket does not push, merge, or close the parent spec)
- **Machine ledger**: [`tui-v1.0.4-capability-ledger.json`](tui-v1.0.4-capability-ledger.json); accounting in [`tui-v1.0.4-capability-ledger.md`](tui-v1.0.4-capability-ledger.md)

## Capability accounting

- 173 index exports, 936 ledger rows: 583 `included`, 348 `cpp-representation`, 5 approved platform/language exclusions. No included row is Deferred or assigned only to close-out.
- `cch_tui_capability_ledger` / `TuiCapabilityLedgerTest.py` pass (structural completeness, frozen revision, cross-references, exclusions).
- Frozen bundle provenance: `python3 scripts/tui/evidence.py verify --baseline pi-v1.0.4` → **36 artifacts verified**.
- All 80 spec sub-issues are closed; the parent spec stays open for the owner.

## Frozen product comparison

- `fixtures/pi-coding-agent/differential/report.json` is re-recorded against pi v1.0.4 (schema 3: cursor position/visibility per snapshot, capture-base provenance with the required #811/#1039 fix commits enforced as ancestors) and verified twice through the CTest gate `cch_native_tui_differential`.
- The 17-scenario triage found **no classification change** from the archived v1.0.0 report (`report-v1.0.0.json`): 7 match rows and 10 recorded product-defect rows.
- `settings-selector` refresh (#1023): `Show hardware cursor` and `Terminal progress` are compared directly (false/true states captured), `Editor padding` and the other pi-only rows remain explicit omissions, and the #809-accepted registry-derived help wording is recorded as an accepted difference. The residual delta is the retained-omission viewport consequence.
- The report was re-recorded after the #1024 default flip, whose only frame change is the now-matching `TUI mode` row (`fullscreen`).

## Full Validation

- Full incremental build `cmake --build --preset vcpkg` on the delivery tip: **140/140 steps, build-phase Parity Architecture Gate PASS** (twice: link phase and gate target).
- Complete unfiltered offline suite `ctest --preset vcpkg` (with `PI_CHECKOUT` so the dual-runtime differential runs): **3603 selected, 3602 passed, 1 by-design skip, 0 failed**, including the **41-test architecture** selection and the real differential gate; total test time 557 s.
  - The single skip is `Native TUI differential capture exposes deterministic Pike cells and ANSI`, the per-scenario capture entry that only runs when the harness exports its scenario environment; the aggregate gate `cch_native_tui_differential` ran and passed.
- Formatting gate `scripts/format-check.sh origin/main` clean; `git diff --check` clean.
- No dependency, toolchain, or configure change in this ticket, so Fresh Validation is not required.

## Removal / migration

- No local-echo path or dock partition remains: `src/tui/ProcessTerminal.cpp` and `src/tui/RenderPipeline.cpp` carry the no-startup-DSR / no-dock-partition contracts, and the #978/#983/#987 tests retain streaming, input, Busy, Close, and preserve-screen behavior through the single renderer output path.
- The fullscreen dock composition is frozen-aligned (#1039: empty idle status plus the widget-region spacer; label at the frozen row), and the mode round trips keep the transcript window and dock rows stable.

## Dated real-terminal evidence

- [#1021](https://github.com/lanshengzhi/cpp-coding-harness/issues/1021) — kitty 0.49.2 (Hyprland/Wayland, 2026-10-18): IME placement and hardware-cursor rows accepted; raw captures in the #1021 notes.
- [#1022](https://github.com/lanshengzhi/cpp-coding-harness/issues/1022) — kitty image protocol (2026-10-18): display, scrolling crop, resize, removal, and switching accepted; recorded divergences stay raw.
- **iTerm2-protocol row: unverified-by-environment** — no iTerm2-protocol emulator exists on this host; recorded as unverified, not passed.

## Remaining risk / open blockers

- [#1043](https://github.com/lanshengzhi/cpp-coding-harness/issues/1043) — residual product-differential deltas without an open tracker (the 72-column startup-help wrap family, the scrollback retained-row delta, the tool-result mid-stream capture artifact, and the settings-selector omission-viewport residual).
- Existing open trackers: [#810](https://github.com/lanshengzhi/cpp-coding-harness/issues/810) (thinking selector), [#812](https://github.com/lanshengzhi/cpp-coding-harness/issues/812) (tool-result presentation), [#831](https://github.com/lanshengzhi/cpp-coding-harness/issues/831) (dock-growth footer frames), [#749](https://github.com/lanshengzhi/cpp-coding-harness/issues/749) (editor layout seam), [#830](https://github.com/lanshengzhi/cpp-coding-harness/issues/830) (renderer commit/rollback protocol).
- This close-out does not claim a full product-differential match; the recorded rows above remain visible instead of silently accepted.

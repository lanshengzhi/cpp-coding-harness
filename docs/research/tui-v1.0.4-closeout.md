# TUI v1.0.4 close-out evidence

- **Baseline**: `pi-v1.0.4` at `7c10bd4337495ee613f2224843ecdf349b80d1df`
- **Authority**: [ADR 0067](../adr/0067-align-the-linux-tui-toolkit-and-native-tui-with-pi-v1-0-4.md), spec [#946](https://github.com/lanshengzhi/cpp-coding-harness/issues/946), close-out ticket [#1025](https://github.com/lanshengzhi/cpp-coding-harness/issues/1025)
- **Delivery branch**: `pike/spec-946-tui-v1.0.4` (the #946 integration branch; the parent spec remains open pending owner acceptance)
- **Machine ledger**: [`tui-v1.0.4-capability-ledger.json`](tui-v1.0.4-capability-ledger.json); accounting in [`tui-v1.0.4-capability-ledger.md`](tui-v1.0.4-capability-ledger.md)

## Capability accounting

- 173 index exports, 936 ledger rows: 583 `included`, 348 `cpp-representation`, 5 approved platform/language exclusions. No included row is Deferred or assigned only to close-out.
- `cch_tui_capability_ledger` / `TuiCapabilityLedgerTest.py` pass (structural completeness, frozen revision, cross-references, exclusions).
- Frozen bundle provenance: `python3 scripts/tui/evidence.py verify --baseline pi-v1.0.4` → **36 artifacts verified**.
- All 80 spec sub-issues are closed; the parent spec stays open for the owner.

## Frozen product comparison

- `fixtures/pi-coding-agent/differential/report.json` is re-recorded against pi v1.0.4 (schema 3: cursor position/visibility per snapshot, capture-base provenance). `captureBase.sha` records `b20f59ad642f0878f90bc919d73fcab545d9d4ea`; the #811/#1024/#1039 fix commits are enforced as ancestors. The final report and aggregate CTest gate verify all 17 report digests against the frozen checkout.
- Final 17-scenario triage: **7 match, 9 product-defect, 1 `capture-timing-artifact`**. The v1.0.4 baseline promotion itself found no category changes; a later triage correction reclassifies the `tool-result` mid-stream snapshot because its post-completion snapshot matches.
- `settings-selector` refresh (#1023): `Show hardware cursor` and `Terminal progress` are compared directly (false/true states captured), `Editor padding` and the other pi-only rows remain explicit omissions, and the #809-accepted registry-derived help wording is recorded as an accepted difference. The retained-omission viewport consequence remains a product difference.
- The report was re-recorded after the #1024 default flip, whose only frame change is the now-matching `TUI mode` row (`fullscreen`). The `scrollback` retained-row delta remains a separate product difference from the 72-column startup-help wrap family.

## Full Validation

- Full incremental build `cmake --build --preset vcpkg --parallel 4`: **47/47 steps; build-phase Parity Architecture Gate PASS**.
- Complete unfiltered CTest suite with absolute `PI_CHECKOUT` and `PI_BASELINE_COMMIT=7c10bd4337495ee613f2224843ecdf349b80d1df`: **3,605 selected, 3,604 passed, 1 skipped, 0 failed**; all 41 architecture tests passed; total test time 239.94 s.
  - The scenario-only Native TUI capture test is the sole expected skip because its scenario environment is unavailable. The aggregate differential gate passed **1/1** and verified all 17 report digests.
- The TUI-labeled CTest selection passed **1,569/1,570**, with the same scenario-only capture test skipped; scheme-listener tests passed **2/2**; architecture selection passed **41/41**.
- Terminal color callbacks run outside the query mutex, and completed queries leave the pending queue before callbacks. The regression test verifies that a late-reply callback can call `stop()` without deadlocking.
- Formatting gate `scripts/format-check.sh origin/main` clean; `git diff --check` clean.
- No dependency, toolchain, or configure change in this ticket, so Fresh Validation is not required.

## Removal / migration

- No local-echo path or dock partition remains: `src/tui/ProcessTerminal.cpp` and `src/tui/RenderPipeline.cpp` carry the no-startup-DSR / no-dock-partition contracts, and the #978/#983/#987 tests retain streaming, input, Busy, Close, and preserve-screen behavior through the single renderer output path.
- The fullscreen dock composition is frozen-aligned (#1039: empty idle status plus the widget-region spacer; label at the frozen row), and the mode round trips keep the transcript window and dock rows stable.

## Dated real-terminal evidence

- [#1021](https://github.com/lanshengzhi/cpp-coding-harness/issues/1021) — kitty 0.49.2 (Hyprland/Wayland, 2026-10-18): IME placement and hardware-cursor rows accepted; raw captures in the #1021 notes.
- [#1022](https://github.com/lanshengzhi/cpp-coding-harness/issues/1022) — kitty image protocol (2026-10-18): display, scrolling crop, resize, removal, and switching accepted; recorded divergences stay raw.
- **iTerm2-protocol row: unverified-by-environment** — no iTerm2-protocol emulator exists on this host; recorded as unverified, not passed.
- Host-local raw captures: `/home/lansy/.local/share/spec-946-notes/` (`issue-1021-*`, `issue-1022-*` reports and their per-scenario directories; not committed to the repository).

## Remaining risk / open blockers

- [#1043](https://github.com/lanshengzhi/cpp-coding-harness/issues/1043) — remaining product-differential deltas without individual trackers: the 72-column startup-help wrap family, scrollback retained-row delta, and settings-selector retained-omission viewport. The tool-result mid-stream row is now classified as a capture-timing artifact; the #1043 issue text predates that correction and needs owner triage.
- `pike/spec-946-1000-parent` still reports **UNAPPLIED (3)** in the branch audit: it holds the initial #1000 ScrollView implementation and two frozen-evidence commits that are patch-unique, while this integration branch contains later ScrollView code and refreshed #1000/#1001/#1004 evidence. The side branch is retained for owner review; no commits were cherry-picked and it was not deleted.
- Existing open trackers: [#810](https://github.com/lanshengzhi/cpp-coding-harness/issues/810) (thinking selector), [#812](https://github.com/lanshengzhi/cpp-coding-harness/issues/812) (tool-result presentation; confirm scope against the timing classification), [#831](https://github.com/lanshengzhi/cpp-coding-harness/issues/831) (dock-growth footer frames), [#749](https://github.com/lanshengzhi/cpp-coding-harness/issues/749) (editor layout seam), [#830](https://github.com/lanshengzhi/cpp-coding-harness/issues/830) (renderer commit/rollback protocol).
- This close-out does not claim a full product-differential match; the recorded rows above remain visible instead of silently accepted.

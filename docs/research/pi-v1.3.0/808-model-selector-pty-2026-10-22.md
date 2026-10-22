# #808 model selector PTY capture — 2026-10-22

This dated transcript records a real PTY drive of `build/dev-fast/pike` at **100 columns × 24 rows**. It exercises Ctrl+L to open the selector and Enter to select the current model.

## Captures

- [Run identity and commands](../../../artifacts/verify-pike/20261022T183349Z-model-selector/identity.txt)
- [Selector after Ctrl+L](../../../artifacts/verify-pike/20261022T183349Z-model-selector/pty-model-selector-open.txt)
- [Frame after Enter](../../../artifacts/verify-pike/20261022T183349Z-model-selector/pty-model-selector-post-enter.txt)
- [Raw PTY ANSI input](../../../artifacts/verify-pike/20261022T183349Z-model-selector/raw.ansi), SHA-256 `05c8d4f4bce765afce2f1888a9610b233c4190b6ea35b49b11416b5077c2ce50`

The open frame shows the selected `→ ✓ faux-1 [magpie]` row, two faux models, the settled catalog-refresh status, a blank row before the save hint, and the lower border immediately after the hint. After Enter, the captured frame shows exactly ` Model: faux-1` on row 3.

The PTY is a real pseudo-terminal driven by `.cursor/skills/verify-pike/helpers/pike_tui.py`; the saved ANSI file contains the terminal byte stream. The helper's text screens are a reconstruction of those bytes using the helper's documented CSI subset, not a general terminal-emulator rendering. The selector was backed by an isolated deterministic local faux catalog/inference fixture, redirected from the product's default Magpie port, so the run did not call an external API.

The binary hash records which executable ran, while `build_source_state = unknown` remains explicit: the available build has no build-time source identity bound to that hash. The actual output root is ignored temporary evidence under `artifacts/verify-pike/`; the linked files are retained there as local run evidence and are not registry artifacts. Parent integration should copy/attach them to its ticket evidence store before this worktree is removed.

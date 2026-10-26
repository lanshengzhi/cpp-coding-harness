# Trust prompt

On a fresh state root, booting the TUI in a directory that has not been trusted
stops at a modal `Trust project folder?` selector. The choice gates whether
project `.pi` resources load; it is remembered per workspace, so later launches
on the same state root skip the prompt.

## Sub-features

- `trust-modal` — the selector appears on first boot for an untrusted folder.
- `trust-noncancellable` — `Escape` and `Ctrl+C` do not dismiss it; a choice is
  required.
- `trust-decision-memory` — the decision persists, so subsequent launches go
  straight to the editor.
- `trust-untrusted-banner` — after `Do not trust`, the TUI shows
  `This project is not trusted. Project .pi resources are ignored. Use /trust to
  save a trust decision, then restart pike.`

## How to get to it (user POV)

- Run `pike` in a folder it has not seen before on this state root.

## Driving it with pike_tui.py / tmux

Preconditions:

- A disposable `HOME`/`XDG_CONFIG_HOME` root is exported.
- The workspace has not been trusted on this state root (a fresh root always
  qualifies).

Drive this feature with the Launch/Drive recipe in `../SKILL.md` — helper or
tmux, and the tmux recipe there owns the run's socket and session. The steps
below name only what is specific to this feature: which keys to send and which
anchors to assert.

- **Boot into the prompt.** Spawn the TUI in the untrusted workspace; the screen
  shows `Trust project folder?` with options `Trust`, `Trust parent folder`,
  `Trust (this session only)`, `Do not trust`, `Do not trust (this session
  only)`. Snapshot the modal as `$EVIDENCE_DIR/trust-modal.txt`.
- **Confirm it does not cancel on Escape.** Send `Escape` (helper `--key
  escape`, tmux `Escape`), snapshot, and assert `Trust project folder?` is still
  on screen. Repeat with `ctrl+c`.
- **Choose `Do not trust`.** The selector opens on `Trust`; send `Down` three
  times, then `Enter`.
- **Assert the editor is reachable.** Expect `Press ctrl+o to show full startup
  help` and the untrusted banner. Snapshot as
  `$EVIDENCE_DIR/trust-dismissed.txt` and grep for both anchors.
- **Proof.** The modal snapshot, the post-dismiss snapshot showing the banner
  and the editor, and a second launch on the same state root going straight to
  the editor together prove the flow.

## Gotchas

- This is a selection list, not a dialog: only `Enter` on an option advances.
  `Escape` and `Ctrl+C` are no-ops here despite the footer hint.
- The prompt only appears on a fresh or reset state root. A previously trusted
  workspace boots straight to the editor, which is itself the
  `trust-decision-memory` proof.
- Trusting a parent folder is a broader grant than the run needs. For
  verification, `Do not trust` is enough to reach every downstream anchor; only
  choose `Trust` when the feature under test reads project `.pi` resources.

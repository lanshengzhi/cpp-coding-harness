# Resume session

A persisted session can be reopened. The TUI `/resume` command opens a session
picker listing prior sessions for the current folder; the `--resume` CLI flag
resumes the most recent session. Selecting one reloads its history.

## Sub-features

- `resume-picker` — `/resume` lists prior sessions with a scope/sort header.
- `resume-recent` — `--resume` reopens the most recent session for the folder.
- `resume-history` — the reopened session shows the prior conversation.

## How to get to it (user POV)

- Run `pike /resume` (or type `/resume` in the TUI) and choose a session.

## Driving it with pike_tui.py

Preconditions:

- A session with a known prompt (e.g. `persist me`) already exists for the
  workspace — run `session-persistence` first.
- Same disposable `HOME`/`XDG_CONFIG_HOME` and workspace `--cwd`.

- **Open the picker.** Spawn `--no-session`, then send `/resume` and Enter:
  `"$HELPER" send --name res --text "/resume"`, `"$HELPER" send --name res --key
  enter`. Expect the header: `"$HELPER" expect --name res --text "Resume Session"`.
- **Read the list.** `"$HELPER" screen --name res --path artifacts/verify-pike/resume-picker.txt`.
  The picker shows a `Resume Session (Current Folder)` header, scope/sort hints
  (`tab scope`, `re:<pattern>`, `ctrl+s sort`), and the prior session rows
  including the known prompt text.
- **Resume via CLI flag.** Spawn with `-- --resume` in the same workspace. With a
  single prior session it is selected automatically and the prompt reappears.
- **Proof.** The picker snapshot shows the persisted session is discoverable;
  the `--resume` snapshot shows its prompt reloaded. Name the resumed session's
  prompt text on the artifact.

## Gotchas

- With exactly one prior session, `--resume` auto-confirms without an
  interactive picker; to exercise the picker itself, drive the in-TUI `/resume`.
- The picker scope is the current folder by default and matches the directory
  pike was launched from. A session persisted under a different workspace key
  (see `session-persistence` gotchas) is invisible in Current Folder scope;
  switch to `Resume Session (All)` with `tab` to reach it.
- A `--no-session` run can still open the picker — `--no-session` only affects
  whether the *new* run persists, not whether it can resume an old session.
- The picker row label is derived from the conversation and may be a single
  character rather than the full prompt text. The path column (toggle with
  `ctrl+p`) is the reliable way to confirm which workspace a row belongs to.

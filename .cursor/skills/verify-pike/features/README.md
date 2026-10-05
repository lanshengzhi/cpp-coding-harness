# Pike verification map

This directory is the maintained source for verifying user-facing Pike
behavior. Read this index before driving the app, then use the matching feature
file as the recipe. The harness is `helpers/pike_tui.py`; the skill entry point
is `.cursor/skills/verify-pike/SKILL.md`.

## Baseline preconditions

- `build/release/pike` exists and `pike --version` exits 0 (build it per
  `SKILL.md` Launch if not).
- A disposable state root: set `HOME` and `XDG_CONFIG_HOME` to a fresh
  `mktemp -d` tree. Pike's Agent Config Directory is `$XDG_CONFIG_HOME/pike/agent`
  and **no env var relocates it** — isolation is via `HOME`/`XDG_CONFIG_HOME`.
- The default proof is **offline and credential-free**: submitting a prompt with
  no model configured deterministically yields `Error: Unknown provider: unknown`
  (exit 1 in `--print`). A real provider round trip needs explicit credentials
  and user authorization.
- Doctor (`SKILL.md`) passes: binary present, `--help` shows `--print`, offline
  error is the deterministic provider error.

## Driving conventions

- Start every recipe from the baseline state unless its preconditions say
  otherwise.
- Drive the TUI through `helpers/pike_tui.py` (`spawn`/`send`/`screen`/`expect`/
  `kill`) or a real tmux pane (`send-keys`/`capture-pane`). Drive the one-shot
  CLI with a plain `env … pike --print` subprocess. Pick one TUI driver per run.
- Prefer stable text anchors (`Available commands:`, `Error: Unknown provider:`,
  `Resume Session (Current Folder)`) over coordinates.
- Treat helper commands as literal; keep quoted names unchanged.
- Clear the editor with `ctrl+c` before typing a slash command; the router only
  dispatches when the buffer starts with `/`.
- On a fresh state root, dismiss the modal `Trust project folder?` prompt by
  selecting `Do not trust` (`Down` `Down` `Down`, then `Enter`). `Escape` and
  `Ctrl+C` do not cancel it.
- One PTY session (or tmux pane) per feature run; isolate concurrent runs by
  `RUN_DIR` and `HOME`/`XDG_CONFIG_HOME`.
- Never drive a Pike instance against the developer's real `~/.config/pike`.

## Proof and skip reporting

- Capture the action and the resulting state, not only the final screen.
- TUI proof: a `screen` snapshot per meaningful step plus the raw `raw.ansi`
  transcript.
- CLI proof: stdout, stderr, and exit code, captured separately.
- Side-effect proof: a new `.jsonl` under
  `$XDG_CONFIG_HOME/pike/agent/sessions/<workspace-key>/` for a session-backed
  run; read its typed JSON lines to confirm the user turn and response.
- Name the feature ID and entry point on every artifact.
- Report an unreachable path with the attempted command and the unmet
  precondition. Do not report a skipped entry point as verified through a
  different path.

## Feature entry contract

Each feature file starts with an H1 title and one paragraph describing the
user-visible behavior, then exactly four H2 sections in order:
`Sub-features`, `How to get to it (user POV)`,
`Driving it with pike_tui.py`, `Gotchas`.

## Features

- [startup-and-slash-commands](startup-and-slash-commands.md) — boot the Native
  TUI, route a slash command, and read the startup banner and footer.
- [session-persistence](session-persistence.md) — hold a conversation in a
  session-backed run and prove the transcript is written under the workspace key.
- [resume-session](resume-session.md) — reopen a persisted session from the TUI
  resume picker or `--resume`.
- [print-one-shot](print-one-shot.md) — the non-interactive `--print` text CLI,
  including the deterministic offline failure.
- [trust-prompt](trust-prompt.md) — the first-run project trust selector and
  its non-cancellable modal behavior.

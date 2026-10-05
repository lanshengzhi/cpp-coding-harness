# Startup and slash commands

Booting the Native TUI paints the startup banner, the footer with the current
working directory and model state, and the input editor. A submission beginning
with `/` is parsed by the built-in slash-command router; `/help` lists the
available commands.

## Sub-features

- `tui-boot` — the Native TUI paints its banner, footer, and editor on a TTY.
- `slash-router` — a `/`-prefixed submission is routed to a built-in command.
- `slash-help` — `/help` prints the available command list.

## How to get to it (user POV)

- Run `pike` with no prompt in a terminal (selects the Native TUI on a TTY).
- Type `/help` at the input editor and press Enter.

## Driving it with pike_tui.py

Preconditions:

- `build/release/pike` is built; Doctor passes.
- A disposable `HOME`/`XDG_CONFIG_HOME` root is exported.
- The helper is on path as `$HELPER`.

- **Boot.** Spawn: `"$HELPER" spawn --name boot --binary "$PWD/build/release/pike"
  --cwd "$VERIFY_ROOT/ws" --env "HOME=$HOME" --env "XDG_CONFIG_HOME=$XDG_CONFIG_HOME"
  -- --no-session`. The screen shows `Press ctrl+o to show full startup help` and a
  footer ending in the cwd and `unknown`. Run `"$HELPER" screen --name boot
  --path artifacts/verify-pike/startup-boot.txt`.
- **Route a slash command.** Send `/help` then Enter: `"$HELPER" send --name boot
  --text "/help"`, `"$HELPER" send --name boot --key enter`. Expect
  `Available commands:`: `"$HELPER" expect --name boot --text "Available commands:"`.
- **Read the command list.** `"$HELPER" screen --name boot --path
  artifacts/verify-pike/startup-help.txt`. The list includes `/login /logout
  /resume /fork /tree /reload /compact /trust`.
- **Proof.** The boot and help snapshots plus the raw `raw.ansi` transcript show
  the banner, the routed command, and its output.

## Gotchas

- Without a model configured, the startup banner shows `Warning: No models
  available.` and any prompt submission surfaces `Error: Unknown provider:
  unknown`. This is expected offline, not a fault.
- The slash router only runs on direct editor submissions in the TUI, not in
  `--print` mode.
- The router dispatches on the first token only. If the editor already holds
  text, typing `/help` appends to it (`x/help`) and the whole buffer submits as
  a literal prompt. Clear the buffer with `ctrl+c` before a slash command.
- A submission that is not a known command falls through to an ordinary Agent
  Prompt; `/help` is the reliable anchor because its output is fixed.
- The command list is wider than an 80-column pane. Use a wide pane (tmux
  `-x 140`) or the helper's default 110 columns so every command stays on one
  row.

---
name: verify-pike
description: >-
  Drive the real `pike` coding-agent Runtime and capture evidence. Use when you
  need to launch the Pike native TUI or its one-shot `--print` CLI, exercise a
  user-facing feature (slash commands, sessions, resume, trust, provider
  errors), and prove the behavior with screenshots/transcripts and side-effect
  checks. Surfaces: interactive Native TUI (a real TTY via the bundled PTY
  helper or tmux) and the non-interactive `--print` text CLI.
---

# Verify Pike

Pike is a C++23 coding-agent Runtime. It has two user-facing surfaces:

- **Native TUI** — interactive fullscreen terminal UI (the default on a TTY).
- **One-shot text CLI** — `--print` (or any non-TTY stream) reads a prompt and
  writes only the final assistant text to stdout, exiting 0 on success and 1 on
  error.

Both are driven against the **real binary**. There is no separate "test mode" or
test-only endpoint; the supported offline harness configures an isolated state
root and relies on deterministic, credential-free provider errors for the
round-trip path. Live-provider round trips are possible but require explicit
user-provided credentials and are out of scope for the default offline proof.

## Using this skill

This is a file in the repository, pointed at from `AGENTS.md`'s Running/E2E row.
**Read the file and follow it.** Nothing auto-loads it, and it is not invocable as
a Cursor skill from here, so "use the verification skill" means open this file.
Read `helpers/` and `features/` from the same path.

## Launch

`pike` is a short-lived process: it starts, runs, and exits. "Launch" means
build the binary once, then start each drive in its own isolated PTY (TUI) or
subprocess (`--print`). There is no long-lived server to keep alive.

**Build once (if `build/release/pike` does not exist or is stale):**

```bash
scripts/bootstrap.sh                 # host precheck + pinned vcpkg (once)
export VCPKG_ROOT="$PWD/.deps/vcpkg"
cmake --preset vcpkg-release
cmake --build --preset vcpkg-release
```

The Runtime binary is `build/release/pike`. A faster-starting Debug binary is at
`build/pike` after `cmake --build --preset vcpkg`; prefer the Release binary for
final evidence, the Debug binary for fast iteration.

**Pick one binary for the whole run and export it.** Every command below, the
Doctor and the run manifest then refer to the same `$BIN`:

```bash
BIN="$PWD/build/release/pike"     # or "$PWD/build/pike" for the Debug binary
export BIN
```

**Isolation (mandatory).** Pike roots all user state at the Agent Config
Directory = `$XDG_CONFIG_HOME/pike/agent` (default `~/.config/pike/agent`).
**No environment variable relocates this root** — `PIKE_CONFIG_DIR` and
`PIKE_CODING_AGENT_DIR` are never read (`src/coding_agent/AgentConfigDir.hpp`,
ADR 0058). To isolate a run, set both `HOME` and `XDG_CONFIG_HOME` to a
disposable directory. Never drive a Pike instance against the developer's real
`~/.config/pike`.

```bash
VERIFY_ROOT="$(mktemp -d /tmp/pike-verify.XXXXXX)"
export HOME="$VERIFY_ROOT/home" XDG_CONFIG_HOME="$VERIFY_ROOT/home/.config"
mkdir -p "$HOME"
```

**TUI (PTY or tmux):** use the bundled PTY harness (see Helpers) or drive a
real tmux pane. The PTY harness spawns `pike` on a pseudo-terminal, owns the
PTY master in a per-session broker, and lets separate processes send keystrokes
and snapshot the visible screen. tmux gives the same real TTY with
`send-keys`/`capture-pane` and is the better fit when a run needs an in-pane
shell (for example to `cd` between launches) or when you want to watch the run
live. Both are first-class; pick one per run and stay on it.


```bash
HELPER=".cursor/skills/verify-pike/helpers/pike_tui.py"
PIKE_VERIFY_RUN_DIR="$VERIFY_ROOT/run" "$HELPER" spawn \
  --name main --binary "$BIN" --cwd "$VERIFY_ROOT/ws" \
  --env "HOME=$HOME" --env "XDG_CONFIG_HOME=$XDG_CONFIG_HOME" \
  -- --no-session          # or omit for a persisted session; add flags after --
```

**tmux:** give the run its **own tmux server**, with the socket inside the
disposable run root, and a plain session name. **Every tmux command in this
skill passes `-S "$TMUX_SOCK"`**, so no other server, socket, or session on the
machine can be addressed — neither by a drive nor by cleanup.

```bash
TMUX_SOCK="$VERIFY_ROOT/tmux.sock"     # inside the disposable root; removed with it
SESS="pike-verify"
tmux -S "$TMUX_SOCK" new-session -d -s "$SESS" -x 100 -y 30 \
  "env HOME=$HOME XDG_CONFIG_HOME=$XDG_CONFIG_HOME $BIN --no-session"
```
Every tmux command in this skill carries `-S "$TMUX_SOCK"`: that one path keeps
this run's server separate and its teardown scoped to itself. Keep `-S` over `-L`:
`tmux(1)` states that with `-S` the default socket directory "is not used and any
`-L` flag is ignored", so the socket stays under the run root that cleanup
removes, and a later `-L` cannot silently redirect it.

Readiness: the startup banner, `Press ctrl+o to show full startup help`, the
footer (`<cwd>` … `unknown`), and the input editor are painted. The harness
returns after the first paint; for tmux, poll `capture-pane` until the banner
appears.

**First-run trust prompt.** On a fresh state root, boot can stop at a modal
`Trust project folder?` selector before the editor is reachable. It is a
selection list, not a dialog: `Escape`/`Ctrl+C` do **not** cancel it. Move with
`Down` to `Do not trust` and confirm with `Enter`. tmux:

```bash
tmux -S "$TMUX_SOCK" send-keys -t "$SESS" Down Down Down   # to 'Do not trust'
tmux -S "$TMUX_SOCK" send-keys -t "$SESS" Enter
```

The trust decision is remembered for the workspace, so later launches on the
same state root skip the prompt. An unattended drive that never clears this
prompt will time out on every anchor below it.

**One-shot CLI (`--print`):** no PTY; a plain subprocess with the isolated env.

```bash
env HOME="$HOME" XDG_CONFIG_HOME="$XDG_CONFIG_HOME" \
  $BIN --print "hello" </dev/null
```

**Teardown:** kill every session you spawned and remove the disposable root.

```bash
# A non-zero `kill` means the session was not reclaimed: stop here and keep the
# root, which holds the state naming what is still running.
if ! PIKE_VERIFY_RUN_DIR="$VERIFY_ROOT/run" "$HELPER" kill --name main; then
  echo "kill failed for main; keeping $VERIFY_ROOT" >&2
  exit 1
fi
# End this run's tmux server BEFORE the root goes away. A socket file means a
# server was started. These snippets assume no `set -e`, so the failure branch is
# written out: a kill that fails must never fall through to the rm -rf below.
if [ -S "$TMUX_SOCK" ]; then
  if ! tmux -S "$TMUX_SOCK" kill-server; then
    echo "kill-server failed for $TMUX_SOCK; keeping $VERIFY_ROOT" >&2
    exit 1
  fi
fi
rm -rf "$VERIFY_ROOT"                                   # then the root, socket file included
```

`kill` exits **4** when the pid recorded for a session is in use but cannot be
identified as that session's: it signals nothing and leaves the state and socket in
place. Treat 4 as **unfinished cleanup, not a clean one**, and stop for any other
non-zero exit too — removing the root would delete the state that says what is
still running.

## Doctor

Run before driving whenever anything looks off. All checks are read-only.

```bash
BIN="${BIN:-$PWD/build/release/pike}"   # the run's binary; Release default
"$BIN" --version                       # prints a version, exit 0
"$BIN" --help | grep -qF -- "--print"  # flag surface present
test -x "$BIN"                          # binary is executable

# Isolation is real: the resolved state root must be under our disposable HOME,
# never the developer's real home.
resolved="$(env HOME="$HOME" XDG_CONFIG_HOME="$XDG_CONFIG_HOME" "$BIN" --print x 2>&1 >/dev/null || true)"
# The offline error must be the deterministic provider error, not a crash.
grep -qF "Unknown provider" <<<"$resolved"
```

A doctor failure means: the binary is missing or unusable (rebuild), the flag
surface drifted (the CLI seam changed — re-read `docs/usage.md`), or state is
leaking into the real home (isolation broken — do not proceed). **Staleness is
deliberately not on this list** — the doctor cannot see it (next paragraph), so
`stale` is a verdict from build-identity evidence outside the doctor, never a
doctor failure.

**The doctor does not judge freshness.** It checks that the binary runs, that the
flag surface is present, and that isolation holds — it cannot tell whether `BIN`
was built from the current sources. `--version` prints the project version only
(currently `0.1.0`), and a stale binary is something the operator infers, not
something these checks detect. Establish build provenance only from a build-time
identity bound to the binary's own hash; that is **not implemented here**. So read
a passing doctor as "the binary runs and isolation holds" — never as "the current
sources are verified".

## Drive

The PTY harness is the primary driver for the TUI. All paths below are relative
to the repo root. Prefer the exact text anchors observed from the real binary.

```bash
HELPER=".cursor/skills/verify-pike/helpers/pike_tui.py"
export PIKE_VERIFY_RUN_DIR="$VERIFY_ROOT/run"
```

- **Snapshot the visible screen**, the primary thing you assert on. PTY helper
  (ANSI-stripped rows): `"$HELPER" screen --name main`. tmux (keeps colors with
  `-e`): `tmux -S "$TMUX_SOCK" capture-pane -t "$SESS" -p -e`. Save under
  `$EVIDENCE_DIR/<feature>-<step>.txt`.

  **Both `screen` and `expect` read one reconstruction**, produced by
  `interpret_screen` in `helpers/pike_tui.py`. It parses the CSI subset that
  helper lists: cursor motion, erase, CR/LF/BS/TAB, and printable text. It is
  **not** a general VT emulator and does **not** handle the alternate screen
  (`\x1b[?1049h` / `l`), so against an application that switches screens the
  reconstruction -- and an `expect` that matches on it -- can be incomplete.
- **Send text / keys.** Helper: `"$HELPER" send --name main --text "/help"`, then
  `"$HELPER" send --name main --key enter`. tmux: `tmux -S "$TMUX_SOCK" send-keys -t "$SESS" -l "/help"`,
  then `tmux -S "$TMUX_SOCK" send-keys -t "$SESS" Enter`. Helper named keys: `enter escape tab
  backspace up down left right ctrl-c ctrl-d ctrl-l ctrl-o`; tmux takes literal
  text via `-l` and control keys as `C-c`, `C-d`, `C-o`, `Enter`, `Escape`, `Tab`.
- **Wait for output.** Helper: `"$HELPER" expect --name main --text "Available commands:" --timeout 10`.
  tmux has no expect; poll `tmux -S "$TMUX_SOCK" capture-pane -t "$SESS" -p | grep -qF "<anchor>"`
  in a short retry loop.

  **`expect` has four outcomes, and they are not interchangeable.** Exit **0**: the
  anchor is present in the rebuilt view. Exit **1**: the timeout expired without it
  appearing there. Exit **2**: the broker could not be reached. Exit **3**: a
  protocol/transport failure -- an empty response, unparseable JSON, or a reply of
  the wrong shape or missing its `found` field. **Only exit 1 is negative evidence
  that the anchor was not there.** Exits 2 and 3 mean the observation did not
  happen; recording either as the anchor's absence turns "could not look" into
  "looked and did not find".
- **Slash dispatch is first-token.** A `/`-prefixed submission routes only when
  the editor buffer starts with `/`. Clear a stale buffer with `ctrl+c` before
  typing a slash command, or the leftover text prepends and the whole buffer
  submits as a literal prompt (and fails offline with the provider error).

Anchors that are stable on the real binary:

| Feature | How to reach it | Anchor to expect on screen |
| --- | --- | --- |
| Startup | spawn a TUI session | `Press ctrl+o to show full startup help`, footer shows the cwd |
| Slash router | type `/help` + Enter | `Available commands:` then the command list (`/clear /new /quit …`) |
| No-provider error | submit any prompt with no model configured | `Error: Unknown provider: unknown` |
| Command list | `/help` | `/login /logout /resume /fork /tree /reload /compact /trust` |
| Resume picker | `/resume` + Enter | `Resume Session (Current Folder)` header, scope/sort hints, session rows |
| Resume scope | `tab` inside the picker | `Resume Session (All)` header; sessions from other workspaces appear |
| Session persistence | submit a prompt in a non-`--no-session` run | a `.jsonl` appears under `$XDG_CONFIG_HOME/pike/agent/sessions/<workspace-key>/` |

To drive a **real provider round trip** (not the default offline path), put a
credential in the isolated `auth.json` or export the provider env var (e.g.
`KIMI_API_KEY`) and select a model (`--model kimi-for-coding`). This sends live
network traffic and spends quota — only with explicit user authorization. For an
offline-but-configured run, point `models.json` `baseUrl` at a local stub to
observe the request without contacting a vendor.

## Evidence

A proof must capture the **action and the resulting state**, not just the final
screen, and must **verify the side effect** alongside what's visible.

- **TUI evidence.** Capture the screen after each meaningful action and save it
  under `$EVIDENCE_DIR/<feature>-<step>.txt`. With the helper:
  `"$HELPER" screen --name main --path "$EVIDENCE_DIR/<feature>-<step>.txt"`;
  the raw ANSI at `$PIKE_VERIFY_RUN_DIR/<name>/raw.ansi` is the **raw input
  record** -- every byte the PTY delivered, as delivered -- and the reconstructed
  `screen` output is a **view derived from it, bounded by the parser described
  under Drive**. Keep the raw record as the audit trail and assert on the view;
  read a view from an application outside that parser's subset as partial rather
  than as equivalent to the real rendering. Read a **negative** result only from
  `expect` exit 1: exits 2 and 3 mean the observation could not be made, so record
  those as a failed observation rather than as a missing anchor.
  With tmux: `tmux -S "$TMUX_SOCK" capture-pane -t "$SESS" -p -e > "$EVIDENCE_DIR/<feature>-<step>.txt"`;
  tmux keeps no separate raw transcript, so capture at every step you may need
  to defend. Keep both forms where they exist.
- **One-shot CLI evidence.** Capture stdout, stderr, and the exit code
  separately (`--print` writes only final assistant text to stdout; errors go to
  stderr with exit 1).
- **Side effects.** The durable side effect of a session run is the persisted
  transcript: assert a new `.jsonl` appeared under
  `$XDG_CONFIG_HOME/pike/agent/sessions/<workspace-key>/`. Inspect it — entries
  are typed JSON lines (`session`, `thinking_level_change`, `message`), and the
  per-message fields (`role`, `content`, `stopReason`, `errorMessage`) are
  nested under a `message` object, not top level. The transcript is the real
  record of the user turn and the assistant/error response.
- **Offline determinism.** The credential-free round trip fails deterministically
  with `Unknown provider: unknown` and exit 1 (print mode). This is a real
  user-path result, not a mock: the provider resolution seam genuinely runs. Do
  not present it as a successful model call.

**Run id, and where evidence lives.** Give every run its own id and its own
evidence directory. `$EVIDENCE_DIR` below is the only place captures go:

```bash
mkdir -p artifacts/verify-pike
RUN_ID="$(date -u +%Y%m%dT%H%M%SZ)-<feature>"
EVIDENCE_DIR="$(mktemp -d "artifacts/verify-pike/$RUN_ID.XXXXXX")"
```

**`<run-id>` must be unique per run, and the directory must be created
atomically.** A timestamp alone does not guarantee it: two runs of the same
feature starting in the same second would take the same name, and `mkdir -p`
would silently reuse the directory — so the second run overwrites the first
run's proof. `mktemp -d` settles uniqueness in one atomic step and keeps the
readable run id as the prefix. (A fixed capture name fails the same way; it is
the same class as the per-run tmux socket.)

Proof artifacts live under a directory you name (e.g. `artifacts/verify-pike/`)
**outside** the disposable run root, so cleanup never deletes them. **That
directory is a temporary holding area, not a home:** when the run ends, attach the
run brief and the relevant captures to the issue, PR, or CI run they support.
**Never commit a run directory** — a run's captures are evidence for one moment,
not a repository asset. `artifacts/` is untracked here, and widening `.gitignore`
is not this skill's business.

### Run identity (L1)

Record a manifest at the start of every run, **before creating any evidence
file**, alongside the captures (for example `$EVIDENCE_DIR/identity.txt`):

```
run_started_at               = <ISO-8601 timestamp>
run_source_revision          = git rev-parse HEAD
checkout_state_at_run_start  = git status --porcelain     # taken BEFORE evidence exists
build_source_state           = unknown unless a build-time identity bound to the binary hash exists
binary_path                  = the $BIN this run drove (resolve to its real path)
binary_version               = <BIN> --version
binary_sha256                = sha256sum <BIN>
commands                     = <the commands this run actually executed>
evidence                     = <paths to the captures this run produced>
```

Take `checkout_state_at_run_start` **before** writing any capture: this run's own
outputs would otherwise show up in a later `git status` and be counted as source
changes. Keep `checkout_state_at_run_start` (what the tree looked like) separate
from `build_source_state` (what the binary came from). A single boolean `dirty`
conflates user material, run output, and source changes, and does not identify
which inputs changed.

**What L1 proves, and what it does not.** It records that *these binary bytes
were run against this checkout state*. It does **not** prove the binary was built
from that revision: `run_source_revision` is where the checkout was, not where
the binary came from. `binary_version` (a project version) and file mtimes cannot
close that gap. **Without a build-time identity bound to the binary's own hash,
write `build_source_state = unknown` — always**, whether or not the checkout was
clean and whether or not the revision matches. A clean checkout at the right
revision records where the sources were, not what the binary was built from; only
an identity emitted by the build itself and tied to the binary hash supports
anything stronger.

## Cleanup

Kill only what you started; never kill by process name.

```bash
# one helper session — a non-zero exit means it was not reclaimed, so stop and
# keep the root: it holds the state that names what is still running.
if ! PIKE_VERIFY_RUN_DIR="$VERIFY_ROOT/run" "$HELPER" kill --name main; then
  echo "kill failed for main; keeping $VERIFY_ROOT" >&2
  exit 1
fi
# all helper sessions for a run root
pkill -F "$VERIFY_ROOT/run"/*/state.json 2>/dev/null || true
# this run's tmux server — a socket file means a server was started, and a failed
# kill must exit here rather than fall through to the rm -rf below
if [ -S "$TMUX_SOCK" ]; then
  if ! tmux -S "$TMUX_SOCK" kill-server; then
    echo "kill-server failed for $TMUX_SOCK; keeping $VERIFY_ROOT" >&2
    exit 1
  fi
fi
# remove the disposable state root (and the tmux socket inside it)
rm -rf "$VERIFY_ROOT"
```

The helper's `kill` terminates the Pike child and the broker for that session. It
exits **4** when the pid its state names is in use but cannot be confirmed as that
session's: it signals nothing and keeps the state and socket, so **4 is unfinished
cleanup, not a clean one**. Any non-zero `kill` stops cleanup here for the same
reason — removing `$VERIFY_ROOT` would delete the state that records what is still
running.

**Hard requirement: end the tmux server before removing the root, on the run's
own socket.** Removing the socket file does not stop the server — `rm -rf
"$VERIFY_ROOT"` deletes the path while the server process keeps running. Use this
order, with `-S "$TMUX_SOCK"` on the kill:

```bash
if [ -S "$TMUX_SOCK" ]; then
  if ! tmux -S "$TMUX_SOCK" kill-server; then
    echo "kill-server failed for $TMUX_SOCK; keeping $VERIFY_ROOT" >&2
    exit 1
  fi
fi
rm -rf "$VERIFY_ROOT"                 # then the root, socket file included
```

**Two cases, two outcomes.** A run that never started tmux leaves no socket file,
the guard skips the kill, and the root is removed. A run that did start a server
and then cannot end it **exits non-zero and keeps the root and socket** — the
failure branch is written out because these snippets do **not** assume `set -e`,
so a bare `kill-server` inside an `if` would otherwise fall through to `rm -rf` and
orphan the server behind a deleted socket.

The order matters in both directions: deleting the socket first leaves `tmux` no
target to reach, and a `kill-server` without `-S` reaches the default socket,
where it can end a server this run never started.
A tmux teardown kills the whole session and every pane in it; exit any live
Pike first (`C-c` twice, then `C-d`) so the pane shell is at a prompt.
`--no-session` TUI runs and `--print` runs leave no transcript; a session-backed
TUI run leaves its `.jsonl` under the disposable root, removed with the root.
**Cleanup removes instances and scratch state, never the evidence** — proof
artifacts under `artifacts/verify-pike/` must survive teardown. After cleanup,
confirm a named artifact still exists.

## Helpers

- `helpers/pike_tui.py` — the PTY driver. Executable (`chmod +x`). Subcommands:
  `spawn`, `send`, `screen`, `expect`, `kill`. Run `helpers/pike_tui.py --help`.
  It ships with the skill; invoke it by path. State per session lives under
  `$PIKE_VERIFY_RUN_DIR/<name>/` (`state.json`, `raw.ansi`, `broker.sock`).
- `tmux` — the real-terminal driver. No helper ships for it; the recipe in
  Launch/Drive is the whole contract. Useful when a run needs an in-pane shell
  or live observation. Requires an installed `tmux` (`tmux -V`). **Address it
  with the run's own socket (`-S "$TMUX_SOCK"`) on every command**; the default
  server is never touched.

## Verification record

A full pass driven through tmux on 2026-10-04 is recorded in
`artifacts/verify-pike/RUN.md`, with per-step captures alongside it. It is the
reference for what a complete run looks like on this binary.

That record predates the L1 manifest above: it carries a date, the binary path
and version, and its captures, but **no source revision and no binary hash**, so
it cannot be tied to the commit it ran against. Treat it as a shape reference,
not as evidence about the current HEAD.

## Feature map

User-facing features live in `features/`, one file per feature, each with
`Sub-features`, `How to get to it (user POV)`, `Driving it with pike_tui.py / tmux`,
and `Gotchas`. Read `features/README.md` first. Seed the map for the feature you
are proving; do not claim a proof for an entry point the map lists but the run
did not exercise.

**When to run a full map pass.** Covering *every* feature — source read plus a
live drive — is maintenance, not ordinary work. Run it only for an explicit drift
audit, after a change to the map or to the drive harness itself, or when the owner
asks for it. An ordinary change verifies only the feature paths it touches. There
is no fixed cadence and nothing here schedules one.

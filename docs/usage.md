# Using C++ Coding Harness

Examples below use the Release build:

```bash
BIN=build/release/cpp_harness
```

Run `$BIN --help` for the complete flag surface.

## Prompts, files, and images

Interactive terminals open the Native TUI. `--print` or a non-TTY input/output stream selects one-shot text output. `--mode text` is the default mode spelling and does not override TTY-based frontend selection.

```bash
$BIN --print "hello"
printf 'hello from stdin' | $BIN --print
$BIN @screenshot.png "describe this image"
$BIN @notes.txt @diagram.webp "compare these files"
```

An `@path` is classified by its content rather than its extension. PNG, JPEG, GIF, and WebP inputs become image content; other readable files become wrapped text. Missing or unreadable inputs fail before the session starts, and empty files are skipped.

Provider-bound images at most 2000×2000 with base64 payloads below 4.5 MiB are preserved. Larger decodable images are resized with a coordinate-mapping hint; inputs that cannot be reduced safely become omission notes.

## Sessions

Without a session-family flag, each run persists a session under the workspace-keyed user directory in `~/.pi/agent/sessions/`.

```bash
$BIN --no-session                              # in-memory Native TUI
$BIN --print --no-session "hello"              # in-memory print run
$BIN --session /tmp/cpp-session.jsonl "hello"  # exact path
$BIN --continue "continue"                     # most recent session
$BIN --resume                                  # session picker
$BIN --fork PATH_OR_ID                         # copy history into a new session
```

`--session-dir DIR` redirects automatic storage. Its precedence is:

1. `--session-dir`;
2. `PI_CODING_AGENT_SESSION_DIR`;
3. `sessionDir` in `~/.pi/agent/settings.json`;
4. the workspace-keyed default.

```bash
$BIN --session-dir /data/sessions --print "hello"
PI_CODING_AGENT_SESSION_DIR=/data/sessions $BIN --print "hello"
```

`--no-session` leaves no transcript and takes precedence over create/resume/continue inputs; it cannot be combined with `--fork`. Session files remain sensitive even though persisted message content is redacted.

## Models and authentication

Built-in and custom models are composed from the runtime catalog and `~/.pi/agent/models.json`. Select with `--model`, optionally qualified as `provider/model`; use `--provider` to narrow an unqualified model pattern.

Kimi Code is built in as provider `kimi-coding`, model `kimi-for-coding`:

```bash
KIMI_API_KEY=... $BIN --model kimi-for-coding "summarize README.md"
```

Credentials can instead be stored in the pi-compatible `~/.pi/agent/auth.json`:

```json
{
  "kimi-coding": { "type": "api_key", "key": "..." },
  "deepseek": { "type": "api_key", "key": "..." }
}
```

Request authentication resolves in this order: `--api-key`, stored `auth.json` credential, provider environment variables, then a configured `models.json` `apiKey`. `--api-key` is process-local and requires an explicit model selection. OAuth providers use `/login [provider]` and `/logout` in the Native TUI.

Kimi's `ANTHROPIC_BASE_URL` and `ANTHROPIC_API_KEY` examples target Anthropic-shaped clients; this harness does not read those variables. Live use sends prompts, selected file content, and tool output to the provider. Keep credentials out of prompts, files, tool-visible content, and logs.

## Agent configuration

User state defaults to `~/.pi/agent/`; set `PI_CODING_AGENT_DIR` to replace that root.

| Path | Purpose |
| --- | --- |
| `auth.json` | API-key and OAuth credentials shared with pi |
| `settings.json` | model, thinking, session, shell, trust, theme, and presentation settings |
| `models.json` | custom providers and model definitions |
| `keybindings.json` | Native TUI bindings |
| `trust.json` | persisted project trust decisions |
| `themes/` | user themes |
| `sessions/` | persisted Agent Sessions |

Project settings in `.pi/settings.json` load only after Project Trust and override user settings. See [keybindings](keybindings.md) for the binding grammar and implemented actions.

## Native TUI commands

| Command | Action |
| --- | --- |
| `/settings` | Open settings. |
| `/model [search]` | Select a model. |
| `/scoped-models` | Configure models used for cycling. |
| `/copy` | Copy the last agent message. |
| `/name <name>` | Name the session. |
| `/session` | Show session information and statistics. |
| `/hotkeys` | Show effective bindings. |
| `/fork` | Fork at a selected user message. |
| `/tree` | Navigate the session tree. |
| `/trust` | Change the project trust decision. |
| `/login [provider]` | Authenticate a provider. |
| `/logout` | Remove an OAuth login. |
| `/new` | Start a new session. |
| `/compact [instructions]` | Compact context, optionally with instructions. |
| `/resume` | Select a session to resume. |
| `/reload` | Reload settings, bindings, skills, prompts, themes, and context files. |
| `/quit` | Shut down cleanly. |

Loaded skills are invoked as `/skill:<name> [instructions]` while Skill Commands are enabled. Unknown slash text passes through as an ordinary prompt. `/clear` is a keybinding rather than a slash command; use `/hotkeys` to see its effective key. Print mode does not dispatch slash commands.

## User Bash

In the Native TUI, a focused-editor submission beginning with `!` runs the remaining text as a shell command in the Session workspace. Its result is saved in Session history and may enter later model context. Use `!!` to save the execution while excluding it from model context.

A bare `!` or `!!` remains an ordinary prompt. `!!!foo` is excluded User Bash running `!foo`. Prefix interpretation applies only to direct editor submissions, not positional prompts, print mode, skills, or prompt-template expansion.

User Bash output is ANSI-stripped and bounded to a 2,000-line/50 KiB tail, with the full truncated output written to an owner-only temporary file. It is not secret-redacted.

## Print-mode outcomes

On success, print mode writes only the final assistant text to stdout and exits 0. Terminal error or abort outcomes write a diagnostic to stderr and exit 1. A run with no prompt prints nothing and exits 0. SIGTERM and SIGHUP dispose the session and exit 143 and 129 respectively.

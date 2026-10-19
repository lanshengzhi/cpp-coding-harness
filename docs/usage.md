# Using Pike

Examples below use the Release build:

```bash
BIN=build/release/pike
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

Without a session-family flag, session storage uses the workspace-keyed `sessions/` directory under the [Agent Config Directory](#agent-configuration). A new session's file is deferred until its first user or assistant message; assembly bookkeeping and User Bash alone do not create it. Forks and resumes retain their existing eager storage behavior ([ADR 0064](adr/0064-defer-the-session-file-to-the-first-user-or-assistant-message.md)).

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
2. `sessionDir` in the Agent Config Directory's `settings.json`;
3. the workspace-keyed default.

```bash
$BIN --session-dir /data/sessions --print "hello"
```

`--no-session` leaves no transcript and takes precedence over create/resume/continue inputs; it cannot be combined with `--fork`. Session files remain sensitive even though persisted message content is redacted.

To migrate an existing pi installation once, use the explicit importer. It copies the old tree into the fixed Agent Config Directory and refuses to overwrite an existing Pike tree ([ADR 0058](adr/0058-fix-the-product-state-root-under-xdg-in-pi-json-shapes.md)):

```bash
$BIN import                         # import from ~/.pi/agent
$BIN import --from /backup/pi-agent
```

Normal Pike startup never consults the old pi directory or its environment overrides.

## Models and authentication

Model requests are delegated to the local Magpie gateway at `http://127.0.0.1:3425`
([ADR 0071](adr/0071-streamline-pike-for-local-magpie-gateway.md)). Magpie supplies the live model
catalog and manages upstream authentication, model routing, account rotation, and rate-limit
failover. Start it with `magpie serve`.

The catalog is discovered at startup and during `/reload`. Pike ignores model `models.json`
entirely, including custom provider entries, key commands, and malformed files; existing files are
left untouched ([ADR 0072](adr/0072-narrow-models-to-a-single-local-llm-router.md)). Select a
gateway model ID with `--model`; an ID containing `/` does not configure a direct cloud provider.
The independent `--provider` option is removed:

```bash
$BIN --model cursor/claude-sonnet-5-5@300k "summarize README.md"
$BIN --model group/sub-worker "refactor module"
```

The local gateway accepts the default `magpie` API key. Effective authentication uses the startup
`--api-key` override, then a nonempty `MAGPIE_API_KEY`, then `magpie`. `--api-key` also works with
default or resumed model selection and is never persisted. Discovery and inference use the same
key and gateway connection; the key is fixed after the first gateway request, including a failed
request ([ADR 0073](adr/0073-concentrate-gateway-operations-in-models.md)). Pike does not read or
write model-provider credentials in `auth.json`. MCP server OAuth remains separate and stores its
state in `mcp-auth.json`; legacy MCP OAuth entries in `auth.json` are imported there when needed.

For a new session, model selection uses the configured settings default, then
`MAGPIE_DEFAULT_MODEL`, then the first catalog model. An empty catalog reports that the local
gateway needs to be started; Pike has no static model fallback. Live use sends prompts, selected
file content, and tool output to the gateway. Keep credentials out of prompts, files, tool-visible
content, and logs.

## Agent configuration

The **Agent Config Directory** is `$XDG_CONFIG_HOME/pike/agent/`, defaulting to `~/.config/pike/agent/`. Only the XDG base determines this root; Pike and pi directory-override variables are not read. Session-storage overrides do not relocate it ([ADR 0058](adr/0058-fix-the-product-state-root-under-xdg-in-pi-json-shapes.md)).

| Path | Purpose |
| --- | --- |
| `auth.json` | legacy pi credentials; old MCP OAuth entries are imported into `mcp-auth.json` |
| `mcp-auth.json` | MCP server OAuth state |
| `settings.json` | model, thinking, session, shell, trust, theme, and presentation settings |
| `models.json` | ignored by Pike; existing pi model configuration is left untouched |
| `keybindings.json` | Native TUI bindings |
| `trust.json` | persisted project trust decisions |
| `themes/` | user themes |
| `sessions/` | persisted Agent Sessions |

Project settings in `.pi/settings.json` load only after Project Trust and override user settings. See [keybindings](keybindings.md) for the binding grammar and implemented actions.

## Native TUI commands

| Command | Action |
| --- | --- |
| `/help` | Show available commands (alias `/commands`). |
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
| `/login [provider]` | Show local Magpie gateway authentication guidance. |
| `/logout` | Show local Magpie gateway authentication guidance. |
| `/new` | Start a new session (alias `/clear`). |
| `/clear` | Start a new session (alias `/new`). |
| `/compact [instructions]` | Compact context, optionally with instructions. |
| `/resume` | Select a session to resume. |
| `/reload` | Reload settings, bindings, skills, prompts, themes, and context files. |
| `/quit` | Shut down cleanly (aliases `/exit` and `/q`). |

Built-in slash submissions are parsed and validated by the Native TUI router. An unrecognized slash submission is sent as an ordinary Agent Prompt, matching pi's fall-through; a validation failure for a known command stays a visible routing error. Registered Prompt Templates, enabled `/skill:<name>` resources, and compatible absolute-path submissions are Agent Prompts for the same reason. Print mode does not dispatch slash commands.

## User Bash

In the Native TUI, a focused-editor submission beginning with `!` runs the remaining text as a shell command in the Session workspace. Its result is saved in Session history and may enter later model context. Use `!!` to save the execution while excluding it from model context.

A bare `!` or `!!` remains an ordinary prompt. `!!!foo` is excluded User Bash running `!foo`. Prefix interpretation applies only to direct editor submissions, not positional prompts, print mode, skills, or prompt-template expansion.

User Bash output is ANSI-stripped and bounded to a 2,000-line/50 KiB tail, with the full truncated output written to an owner-only temporary file. It is not secret-redacted.

## Print-mode outcomes

On success, print mode writes only the final assistant text to stdout and exits 0. Terminal error or abort outcomes write a diagnostic to stderr and exit 1. A run with no prompt prints nothing and exits 0. SIGTERM and SIGHUP dispose the session and exit 143 and 129 respectively.

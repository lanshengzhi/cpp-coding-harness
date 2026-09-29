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

Without a session-family flag, each run persists a session under the workspace-keyed user directory in `~/.pike/agent/sessions/`.

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
2. `sessionDir` in `$XDG_CONFIG_HOME/pike/agent/settings.json`;
3. the workspace-keyed default.

```bash
$BIN --session-dir /data/sessions --print "hello"
```

`--no-session` leaves no transcript and takes precedence over create/resume/continue inputs; it cannot be combined with `--fork`. Session files remain sensitive even though persisted message content is redacted.

To migrate an existing pi installation once, use the explicit importer. It reads the old tree and refuses to overwrite an existing Pike tree:

```bash
$BIN import                         # ~/.pi/agent -> ~/.pike/agent
$BIN import --from /backup/pi-agent --to ~/.pike/agent
```

Normal Pike startup never consults the old pi directory or its environment overrides.

## Models and authentication

Built-in and custom models are composed from the runtime catalog and `~/.pike/agent/models.json`. Select with `--model`, optionally qualified as `provider/model`; use `--provider` to narrow an unqualified model pattern.

A custom `models.json` provider key also selects that provider's wire protocol behavior. Name the entry after the service it fronts — `openrouter`, `deepseek`, `openai` — whatever `baseUrl` points at, so a gateway or relay in front of a vendor keeps that vendor's request fields; an entry named something else is treated as a plain OpenAI-compatible endpoint. `baseUrl` selects where the request goes, never what it contains.

Kimi Code is built in as provider `kimi-coding`, model `kimi-for-coding`:

```bash
KIMI_API_KEY=... $BIN --model kimi-for-coding "summarize README.md"
```

OpenRouter account authorization is available alongside its API-key
environment variable:

```text
/login openrouter
```

Credentials can instead be stored in `~/.pike/agent/auth.json`:

```json
{
  "kimi-coding": { "type": "api_key", "key": "..." },
  "deepseek": { "type": "api_key", "key": "..." }
}
```

Request authentication resolves in this order: `--api-key`, stored `auth.json` credential, provider environment variables, then a configured `models.json` `apiKey`. `--api-key` is process-local and requires an explicit model selection. OAuth providers use `/login [provider]` and `/logout` in the Native TUI.

Kimi's `ANTHROPIC_BASE_URL` and `ANTHROPIC_API_KEY` examples target Anthropic-shaped clients; this harness does not read those variables. Live use sends prompts, selected file content, and tool output to the provider. Keep credentials out of prompts, files, tool-visible content, and logs.

## Agent configuration

User state lives under `$XDG_CONFIG_HOME/pike/agent/` (defaulting to `~/.config/pike/agent/`); no environment variable relocates it.

| Path | Purpose |
| --- | --- |
| `auth.json` | API-key and OAuth credentials |
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
| `/help` | Show available commands (alias `/commands`). |
| `/settings` | Open settings. |
| `/model [search]` | Select a model. |
| `/scoped-models` | Configure models used for cycling. |
| `/copy` | Copy the last agent message. |
| `/name <name>` | Name the session. |
| `/session` | Show session information and statistics. |
| `/mcp` | Show Upstream MCP Server connection status. |
| `/hotkeys` | Show effective bindings. |
| `/fork` | Fork at a selected user message. |
| `/tree` | Navigate the session tree. |
| `/trust` | Change the project trust decision. |
| `/login [provider]` | Authenticate a provider. |
| `/logout` | Remove an OAuth login. |
| `/new` | Start a new session (alias `/clear`). |
| `/clear` | Start a new session (alias `/new`). |
| `/compact [instructions]` | Compact context, optionally with instructions. |
| `/resume` | Select a session to resume. |
| `/reload` | Reload settings, bindings, skills, prompts, themes, and context files. |
| `/quit` | Shut down cleanly (aliases `/exit` and `/q`). |

Built-in slash submissions are parsed and validated by the Native TUI router. An unrecognized slash submission is sent as an ordinary Agent Prompt, matching pi's fall-through; a validation failure for a known command stays a visible routing error. Registered Prompt Templates, enabled `/skill:<name>` resources, and compatible absolute-path submissions are Agent Prompts for the same reason. Print mode does not dispatch slash commands.

## Upstream MCP Server tools

An Upstream MCP Server configured with `"activation": "eager"` publishes its catalog into the model tool surface as soon as it connects, under a Qualified Tool Name `mcp__<server id>__<tool>`. Discovery never blocks session start: a server that withholds its catalog simply has no tools yet, and the next model request is the first that carries them. A `"lazy"` server — the default — publishes nothing. `/mcp` shows each server's connection status.

A server configured with `"approval": "ask"` (the default is `"allow"`) asks before each of its calls runs. The prompt shows the Qualified Tool Name, the Upstream it targets, and the arguments the call would send, and offers two answers: *allow this call* or *deny this call*. Dismissing it (Escape) is not a decision — nothing is remembered, and the next call asks again. One consent runs exactly one call. A declined, dismissed, or unaskable call is one failed tool call the model is told about, never a session failure: a session with no way to ask — a headless run, or the Native TUI closing under the prompt — refuses the call rather than running it unasked. An `"allow"` server, and every built-in tool, are unaffected and never reach the prompt.

Cancelling a prompt (`Esc` while a turn is running) stops the work an in-flight upstream call asked for: the response stream is closed and the server is sent a `notifications/cancelled` for that call's request, so it need not run the call to its own conclusion. The call itself settles as one failed tool call — the session itself is not affected and the next prompt runs normally. A long upstream operation that reports progress shows that progress in its tool-execution block while it runs; the progress is display-only and costs the model nothing, and the settled result is the Upstream's own output.

## User Bash

In the Native TUI, a focused-editor submission beginning with `!` runs the remaining text as a shell command in the Session workspace. Its result is saved in Session history and may enter later model context. Use `!!` to save the execution while excluding it from model context.

A bare `!` or `!!` remains an ordinary prompt. `!!!foo` is excluded User Bash running `!foo`. Prefix interpretation applies only to direct editor submissions, not positional prompts, print mode, skills, or prompt-template expansion.

User Bash output is ANSI-stripped and bounded to a 2,000-line/50 KiB tail, with the full truncated output written to an owner-only temporary file. It is not secret-redacted.

## Print-mode outcomes

On success, print mode writes only the final assistant text to stdout and exits 0. Terminal error or abort outcomes write a diagnostic to stderr and exit 1. A run with no prompt prints nothing and exits 0. SIGTERM and SIGHUP dispose the session and exit 143 and 129 respectively.

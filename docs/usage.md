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
| `/mcp auth <server>` | Authorize a remote Upstream MCP Server in your browser. |
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

An Upstream MCP Server configured with `"activation": "eager"` publishes its catalog into the model tool surface as soon as it connects, under a Qualified Tool Name `mcp__<server id>__<tool>`. Discovery never blocks session start: a server that withholds its catalog simply has no tools yet, and the next model request is the first that carries them. `/mcp` shows each server's connection status.

A `"lazy"` server — the default — keeps its tools out of the model context until the model asks for one. Once a lazy server is connected, two built-in tools appear in the model's tool list: `mcp_search` returns the discovered tools as compact lines (server id, qualified name, one-line description, never a schema), and `mcp_activate` adds one of them to the model's context for the rest of the session. Activation needs no approval — it makes no call to the server — and takes effect at the next turn; an `"approval": "ask"` server still asks before an activated tool is *called*. Activation is recorded in the session transcript and restored when the session is resumed, and a server's own `instructions` are surfaced to the model as part of the system prompt.

A server configured with `"approval": "ask"` (the default is `"allow"`) asks before each of its calls runs. The prompt shows the Qualified Tool Name, the Upstream it targets, and the arguments the call would send, and offers two answers: *allow this call* or *deny this call*. Dismissing it (Escape) is not a decision — nothing is remembered, and the next call asks again. One consent runs exactly one call. A declined, dismissed, or unaskable call is one failed tool call the model is told about, never a session failure: a session with no way to ask — a headless run, or the Native TUI closing under the prompt — refuses the call rather than running it unasked. An `"allow"` server, and every built-in tool, are unaffected and never reach the prompt.

A remote Upstream MCP Server that answers with an authentication challenge moves to `needs_auth` in `/mcp`, and `/mcp auth <server>` is how you authorize it. The command discovers the server's authorization server from the challenge it sent, registers a client, and shows you an authorization URL — opening a browser best-effort and leaving the link on screen either way. You finish in the browser; the flow returns to pike on its own, and the connection is attempted again with the credential it just stored.

The credential is keyed by the authorization server that issued it, and the authorization response's issuer is checked against the one the request was sent to: a response from anywhere else is refused before its code is exchanged, and nothing is stored. Neither the access token nor the refresh token appears in a status line, a diagnostic, or a transcript. Dismissing the dialog stores nothing and leaves the server `needs_auth`; so does a failure. A session with no terminal to show the URL in — a headless run, print mode — cannot authorize anything and says so rather than waiting for a browser.

Cancelling a prompt (`Esc` while a turn is running) stops the work an in-flight upstream call asked for: the response stream is closed and the server is sent a `notifications/cancelled` for that call's request, so it need not run the call to its own conclusion. The call itself settles as one failed tool call — the session itself is not affected and the next prompt runs normally. A long upstream operation that reports progress shows that progress in its tool-execution block while it runs; the progress is display-only and costs the model nothing, and the settled result is the Upstream's own output.

If an Upstream suspends a tool call to ask for user input, pike shows a Pending Elicitation instead of failing the call. The same dialog chrome, the same three answers, and the same bounded wait serve both modes. In **URL mode** the dialog names the Upstream and the tool, shows the address to visit, offers an open-browser action, and waits for you to press **Done** (`Enter`), **Decline** (`Alt+D`), or **Cancel** (`Esc`) — opening the browser is not an answer, so the dialog stays up and the question stays open until you decide. In **form mode** the dialog renders the fields the Upstream's own JSON Schema declares, each with its own constraint, and you type into the focused one: `Tab`, `Shift+Tab`, and the up and down keys move between fields, and `Enter` submits the whole form. A value the schema rejects is refused in the dialog — the reason appears under that field and focus moves to it — so nothing invalid is ever sent. Values are sent with the type the schema declared, so a field the server called a number arrives as a number. An optional field you leave empty is left out of the answer entirely. A form is too large, too deeply nested, or too long for the dialog to draw, and pike says so on screen and tells you that your answer will carry no fields, rather than showing a partial form as though it were the whole question. Accept and Decline continue the original call with your answers; Cancel tells the Upstream you dismissed it. The wait is bounded and stops with the session, so a suspended call never hangs forever, and a server that asks a type pike does not support fails just that one call.

## User Bash

In the Native TUI, a focused-editor submission beginning with `!` runs the remaining text as a shell command in the Session workspace. Its result is saved in Session history and may enter later model context. Use `!!` to save the execution while excluding it from model context.

A bare `!` or `!!` remains an ordinary prompt. `!!!foo` is excluded User Bash running `!foo`. Prefix interpretation applies only to direct editor submissions, not positional prompts, print mode, skills, or prompt-template expansion.

User Bash output is ANSI-stripped and bounded to a 2,000-line/50 KiB tail, with the full truncated output written to an owner-only temporary file. It is not secret-redacted.

## Print-mode outcomes

On success, print mode writes only the final assistant text to stdout and exits 0. Terminal error or abort outcomes write a diagnostic to stderr and exit 1. A run with no prompt prints nothing and exits 0. SIGTERM and SIGHUP dispose the session and exit 143 and 129 respectively.

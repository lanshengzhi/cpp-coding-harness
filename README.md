# C++ Coding Harness

A small experimental C++23 coding-agent harness built around anti-fragile architecture rules:

1. **Data is passive value state**: public contracts are aggregate-friendly structs, `std::variant` alternatives, `std::expected` failures, and a project `JsonValue` for unstructured JSON facts.
2. **Capabilities cross physical seams**: chat clients, stream transports, execution environments, session stores, and tools are replaceable interfaces or dependency-heavy concrete implementations hidden behind headers.
3. **Events are weak connections**: agent/provider event sinks use move-only callback semantics so subscribers can own unique state without forcing `std::shared_ptr` or copyability.
4. **Generic machinery stays local**: Glaze DTOs, schema conversion, visitors, parsing helpers, and future reflection-friendly machinery live in serialization/implementation layers rather than domain-facing APIs.

The harness loop mirrors the core pi-style flow:

1. accept a prompt from the Native TUI, one-shot CLI, JSON/RPC, or SDK,
2. send ordered messages plus JSON Schema tool definitions to an OpenAI-compatible chat API,
3. execute local tools requested through `tool_calls`,
4. append tool-result messages with matching call IDs,
5. repeat until the assistant stops or an explicitly configured max-turn limit is reached (there is no default cap),
6. persist the redacted typed transcript as JSONL.

This is a learning and experimentation harness, not a production sandbox or compatibility-preserving SDK.

## Build

The project is CMake-based and requires a C++23-capable compiler. CMake 3.25 or newer is expected.

- Glaze is used only at typed JSON serialization/deserialization boundaries.
- utf8proc provides versioned Unicode grapheme segmentation and width properties inside the private TUI implementation.
- MD4C provides tolerant Markdown parsing behind the public TUI component boundary.
- Pinned stb image decode/resize/write headers are vendored under `third_party/stb` and compiled only by the private initial-image processor; their upstream license notices remain in each header. libwebp supplies private WebP validation/decoding for the same processor.
- Boost.Beast/Asio + OpenSSL provide the HTTPS transport implementation.
- Boost.Process is used behind the process-execution capability boundary.
- CLI11 and Catch2 are declared in `vcpkg.json`; this repository also carries a tiny Catch-compatible fallback test header so the default suite can run in minimal environments.

### Bootstrap with vcpkg (recommended)

All dependencies are declared in `vcpkg.json`. The bootstrap scripts create a local `.deps/vcpkg` checkout when `VCPKG_ROOT` is not already set, bootstrap vcpkg, and configure CMake in manifest mode so dependencies such as CLI11, Glaze, MD4C, libwebp, Boost, and OpenSSL are installed automatically.

Linux/macOS:

```bash
scripts/bootstrap.sh --test
```

Windows PowerShell:

```powershell
.\scripts\bootstrap.ps1 -Test
```

Useful options:

- `--no-build` / `-NoBuild` configures dependencies and CMake without compiling.
- `--release` / `-Release` uses the release preset.
- `--vcpkg-root DIR` / `-VcpkgRoot DIR` uses an explicit vcpkg checkout instead of `$VCPKG_ROOT` or `.deps/vcpkg`.

Manual vcpkg usage is also supported when `VCPKG_ROOT` is already set:

```bash
cmake --preset vcpkg
cmake --build --preset vcpkg
ctest --preset vcpkg
```

### Using system packages

If you prefer system-installed dependencies, install Boost, OpenSSL, Glaze, MD4C, libwebp, CLI11, and utf8proc yourself, then use the system preset:

```bash
cmake --preset system
cmake --build --preset system
ctest --preset system
```

Run the binary with the deterministic fake provider. With no `--session`, `--resume`, or `--no-session`, each run persists a new session under the workspace-keyed user-level default (see [Session storage](#session-storage)):

```bash
./build/cpp_harness --fake
./build/cpp_harness --fake "hello"
./build/cpp_harness --fake --print --workspace . "read README.md"
./build/cpp_harness --fake --mode json "hello" | jq -c 'select(.type == "message_update")'
printf '{"type":"get_state"}\n{"type":"shutdown"}\n' | ./build/cpp_harness --fake --mode rpc
printf 'hello from stdin' | ./build/cpp_harness --fake
./build/cpp_harness --fake @screenshot.png "describe this image"
./build/cpp_harness --fake @notes.txt @diagram.webp "compare these files"
```

Positional `@path` arguments are content-sniffed rather than classified by extension. PNG, JPEG, GIF, and WebP image bytes contribute an absolute `<file name="…"></file>` reference to the initial text and an ordinary `ai::ImageContent` block after that complete text block. Non-image files contribute their text inside the same absolute-path wrapper. A lone image still creates the wrapper text and therefore uses the existing text-plus-images Agent Session operation; there is no image-only prompt API.

Provider-bound images are preserved unchanged while dimensions are at most 2000×2000 and the base64 payload is strictly below 4.5 MiB. Larger decodable PNG, JPEG, GIF, and WebP inputs are aspect-ratio resized and carry the baseline coordinate-mapping hint in their `<file>` reference; an input that cannot be safely reduced is represented by an omission note rather than malformed image content. Empty files are skipped, while a missing or unreadable initial file fails before Session Publication.

Pass `--no-session` to run any frontend in memory with no transcript left behind:

```bash
./build/cpp_harness --fake --no-session
./build/cpp_harness --fake --print --no-session "hello"
```

Explicit create and resume paths remain available and may live anywhere:

```bash
./build/cpp_harness --fake --session /tmp/cpp-session.jsonl "hello"
./build/cpp_harness --fake --resume /tmp/cpp-session.jsonl "continue"
```

Redirect automatic storage for one run or for all runs (highest precedence first: `--session-dir`, `CCH_CODING_AGENT_SESSION_DIR`, `sessionDir` in `~/.cpp-harness/agent/settings.json`):

```bash
./build/cpp_harness --fake --session-dir /data/sessions "hello"
CCH_CODING_AGENT_SESSION_DIR=/data/sessions ./build/cpp_harness --fake "hello"
```

The coroutine/Glaze/event stack is the only active stack. Legacy compatibility flags such as `--async` are intentionally absent.

Real-provider mode is OpenAI Chat Completions-compatible:

```bash
export OPENAI_API_KEY=...
./build/cpp_harness --model gpt-4.1-mini "summarize README.md"
```

Use `--base-url` for compatible gateways that preserve the `/v1/chat/completions` contract.

OAuth, subscription-provider, and dynamic API-key resolution flows are intentionally deferred. The `--api-key-env` mechanism reads a static environment variable at provider construction time; there is no OAuth handshake, token refresh, or per-call key callback yet.

### Agent config directory

User-level state lives in the agent config directory `~/.cpp-harness/agent/`, mirroring pi's `~/.pi/agent/`: `auth.json` (static API keys), `settings.json` (provider defaults, theme selection, and resource policy), `keybindings.json` (Native TUI bindings), `trust.json` (persisted project trust decisions), `themes/` (global Native TUI themes), and `sessions/<encoded-workspace>/` (default persisted Agent Session histories). Set `CCH_CODING_AGENT_DIR` to override the location (pi: `PI_CODING_AGENT_DIR`). These paths are centralized in `include/cch/coding_agent/AgentConfigDir.hpp`; theme and keybinding discovery never fall back to `~/.pi`.

### Session storage

Without `--session` or `--resume`, the CLI persists each new session under `<agent config directory>/sessions/<encoded workspace>/<UTC timestamp>_<session id>.jsonl`, mirroring pi's layout. The workspace key is pi's readable encoding of the canonical physical workspace path: an explicit `--workspace` — not the process working directory — selects the location, and symbolic-link aliases of one workspace share one session directory. The filename UUID matches the Session ID stored in the session header and reported by runtime state.

`--session PATH` and `--resume PATH` keep their exact paths and may live outside the default root; automatic-directory overrides never rewrite them. Old project-local `.cpp-harness/sessions/` files are neither scanned nor migrated; pass such a file explicitly through `--resume` if it is still needed. A missing or unwritable default store fails before any model work with the attempted path and reason — the harness never falls back to a workspace-local or temporary transcript. On POSIX, harness-created session directories are owner-only (`0700`) and new session files are `0600`.

Automatic-directory overrides replace the whole automatic directory for default creation: the session file lands directly in the override directory without a workspace-key component (pi: `--session-dir`). The precedence is exactly `--session-dir`, then `CCH_CODING_AGENT_SESSION_DIR`, then `sessionDir` in `~/.cpp-harness/agent/settings.json`, then the workspace-keyed default. Absolute values stay absolute, a leading `~` expands to the home directory, and relative values resolve against the final canonical workspace rather than the launch directory. An override directory that does not exist is created owner-only; an existing override directory keeps its mode while files inside remain `0600`. These CLI-only inputs never affect SDK default persistence.

`--no-session` selects an explicit in-memory Agent Session in every frontend (Native TUI, text one-shot, JSON, and RPC): provider, tools, events, live state, errors, and shutdown behave normally, but no sessions root, workspace-local session directory, or transcript file is created — including after failures. It cannot be combined with `--session` or `--resume`. In-memory operation happens only when explicitly requested; storage unavailability is never silently treated as `--no-session`. `/session` prints the persisted file path, or `File: In-memory` for these runs.

### Auth file

API keys can also be stored in `~/.cpp-harness/agent/auth.json` and selected by name with the `--auth` flag (or the `auth` field in `~/.cpp-harness/agent/settings.json`). This avoids exporting keys into the shell environment.

```json
{
  "kimi-coding": { "type": "api_key", "key": "..." },
  "deepseek": { "type": "api_key", "key": "..." }
}
```

```bash
# Uses the kimi-coding auth entry, no env var needed
./build/cpp_harness --auth kimi-coding \
  --base-url https://api.kimi.com/coding/v1 \
  --model kimi-for-coding \
  "summarize README.md"
```

Auth file entries take precedence over environment variables. `api_key_env` is still used as a fallback and to decide which environment variable names are filtered from child shell processes.

### Kimi Code via the OpenAI-compatible path

Kimi Code can be used through the existing OpenAI-compatible provider path. Keep the Kimi base URL, model, and API key together — whether via `--api-key-env` or `--auth` — because the bearer token is sent to whichever `--base-url` you configure.

```bash
export KIMI_API_KEY=...
./build/cpp_harness \
  --base-url https://api.kimi.com/coding/v1 \
  --model kimi-for-coding \
  --api-key-env KIMI_API_KEY \
  "summarize README.md"
```

Pass the base URL (`https://api.kimi.com/coding/v1`), not the full `/chat/completions` endpoint; the harness appends `/chat/completions` for `/v1`-style base URLs.

Kimi's `ANTHROPIC_BASE_URL` / `ANTHROPIC_API_KEY` examples are for Anthropic-shaped Claude Code clients. This harness does not read those variables or implement an Anthropic provider.

Live Kimi usage sends prompts, file contents read by tools, and tool outputs to the configured provider. JSONL session redaction is a persistence boundary, not a guarantee that terminal output, CI logs, provider diagnostics, or provider-bound tool results are redacted. Do not paste raw credentials into prompts, files, or tool-visible content.

`--resume` reconstructs the redacted active session path, including persisted leaf and compaction context, and restores workspace metadata. When you omit `--model`, `--base-url`, `--api-key-env`, or `--auth`, the harness falls back to values stored in the resumed session, then to `~/.cpp-harness/agent/settings.json`, then to built-in defaults. Explicit CLI flags always win. For Kimi sessions, repeating the Kimi base URL, model, and key source on resume is still recommended so runtime context stays explicit.

Troubleshooting:

| Symptom | Check |
| --- | --- |
| `missing API key` | Export `KIMI_API_KEY` and pass `--api-key-env KIMI_API_KEY`, or add the key to `~/.cpp-harness/agent/auth.json` and pass `--auth kimi-coding`. |
| Authentication or authorization failure | Confirm the key is valid for Kimi Code and that the base URL is `https://api.kimi.com/coding/v1`. |
| Invalid model | Use `--model kimi-for-coding`. |
| Rate limit or quota error | Retry later or check Kimi Code subscription/entitlement and quota. |
| Request unexpectedly goes to OpenAI | Ensure the Kimi `--base-url`, `--model`, and key source (`--api-key-env` or `--auth`) are all present. |
| 403 Forbidden | Your key can list models but is not entitled for Kimi Code chat completions; confirm Kimi Code subscription/agent access. |
| Provider or transport error | Re-run with harmless prompts and inspect diagnostics without printing secrets. |

Optional live smoke validation is manual and never part of default `ctest`:

```bash
CCH_LIVE_KIMI=1 KIMI_API_KEY=... scripts/kimi_live_smoke.sh
```

The smoke script requires explicit opt-in, uses a throwaway workspace/session, does not enable bash, and consumes real network/quota.

## Architecture boundaries

The code is split into value contracts, capability seams, implementation adapters, and package-style CMake targets:

- **`include/cch/`** — passive domain contracts, abstract capability seams, and the embeddable SDK surface (`Sdk.hpp`, `Message.hpp`, `StreamTransport.hpp`, etc.). Public headers do not include Glaze, wire transport, or disk loader/formatting details.
- **`src/`** — implementation adapters kept private via CMake `PRIVATE` include paths: Glaze JSON/DTO serialization (`src/util/Json.hpp`, `src/ai/glaze/`), provider wire transport (`BoostBeastStreamTransport`, `SseParser`), and coding-agent loaders/formatters (`SkillLoader`, `PromptTemplateLoader`, `SkillFormatting`).

Package targets and responsibilities:

- `cch_util` (`include/cch/util`, `src/util`): project error/expected contracts, move-only callback vocabulary, passive `JsonValue`, the Glaze-backed JSON adapter in `src/util/Json.hpp`, and async process execution.
- `cch_tui` (`include/cch/tui`, `src/tui`): reusable source-level terminal UI contracts, a width-bounded structured Component seam, TUI root, semantic key and bracketed-paste input, a Unicode Editor with caller-supplied autocomplete and generic injected styling, Text, injected-style Markdown with optional syntax highlighting, filterable Select List and Settings List interactions, Loader and exactly-once Cancellable Loader lifecycles, structured inline Image placement with bounded fallback and private Kitty/iTerm2 protocol fixtures, a deterministic Virtual Terminal, and the transactional Linux/macOS Process Terminal used by the production Native TUI. The package depends only on project utility contracts, exposes no third-party types, has no coding-agent dependency, and makes no ABI-stability promise.
- `cch_coding_agent_tui` (`src/coding_agent/tui`): the physically separate coding-agent Native TUI presentation layer. Its current supported slices provide baseline-pinned themes and keybindings: pi-compatible theme/keybinding parsing, deterministic Agent Config Directory discovery, bounded diagnostics, capability-aware theme mapping, an immutable effective binding registry shared by dispatch/help/hints, and settings/help presentation adapters. Known application actions are registered only when a frontend concretely assembles them, so deferred actions never become no-op bindings. Discovery is a one-shot startup operation with no generalized hot reload. This target remains independent of the Agent Session runtime.
- `cch_coding_agent_interactive` (`src/coding_agent/tui/InteractiveMode.*`): the private Native TUI composition that connects a real Agent Session to the reusable TUI, Editor, themes, and effective keybindings. It renders the initial Agent Session snapshot before focusing input; reduces user, assistant text/thinking, correlated tool, custom, and summary lifecycle state into one coherent Markdown-capable transcript; composes shared message images through stable reusable `cch::tui::Image` instances in source order; keeps thinking/tool expansion as local presentation state; dispatches one effective command registry before prompt interpretation; derives command autocomplete and help from that registry; opens the supported theme-settings and effective-hotkeys overlays; submits unmatched idle input asynchronously; routes active-run Enter and Alt+Enter directly to Agent Session steering and follow-up admission; visibly presents Agent-owned pending queues and restores them to the editor through the baseline dequeue action; routes the effective interrupt binding to one idempotent Agent Session abort request after editor/autocomplete handling; waits for abort quiescence before accepting another prompt or restoring the terminal; and restores Virtual or Process Terminals on exit. When an asynchronous clipboard reader is injected, its paste action writes sniffed PNG/JPEG/GIF/WebP bytes to a unique `pi-clipboard-<UUID>.<ext>` file under the OS temporary directory and inserts only that path at the editor cursor, falling back silently to clipboard text. The paste handler intentionally does not delete successful files or create attachment preview/removal state. The executable selects this composition for supported interactive terminals and keeps the runtime library, SDK, text, JSON, and RPC paths independent of TUI dependencies.
- `cch_ai` (`include/cch/ai`, `src/ai`): passive message/content/tool/context contracts, provider-neutral stream events, provider registry, OpenAICompletionsCompat flags, OpenAI-compatible provider, scripted fake provider, and prompt cancellation propagation through provider transport; SSE and Glaze provider mapping live under `src/ai/`.
- `cch_agent` (`include/cch/agent`, `src/agent`): public stateful `Agent` ownership of live message history, model/thinking/tool state, weak move-only subscriptions with bounded diagnostics, passive state snapshots, one active run, and the strong per-run commitment seam used by Agent Session persistence. The package also owns async tool registration, private Tool Argument Contract preparation, expected-style tool execution, pi-ordered prepare/stop/steering/follow-up policy seams, and sequential/bounded-parallel tool execution policy.
- `cch_harness` (`include/cch/harness`, `src/harness`): pi-shaped filesystem and shell execution capability contracts (`FileSystem`/`Shell`), local implementation with workspace containment, symlink safety, atomic writes, split-stream process execution, secret environment filtering, and JSONL/in-memory Session Store implementations.
- `cch_tools` (`include/cch/tools`, `src/tools`): built-in read/write/edit/bash tool factories bridging agent tool contracts to harness capabilities.
- `cch_coding_agent_runtime` (`src/coding_agent/runtime/`, `include/cch/coding_agent/`, `src/coding_agent/`, and private one-shot adapters in `src/cli/`): SessionFactory-authoritative session assembly (including `settings.json` precedence), agent config directory path resolution (`AgentConfigDir`), user settings and auth loading (`SettingsLoader`, `AuthLoader`), session lifecycle, composition of the stateful Agent with persistence/resources/SDK presentation, semantic event printing, one-shot/JSON/RPC output, private skill/template prompt interpretation (`prompt/PromptProcessor`), strong incremental message persistence after Agent state and weak observers advance, project trust/resource controls (`ProjectTrust`, `ProjectResources`, `ProjectResourceLoader`), project-local skill discovery/loading and prompt formatting, `/skill:name` expansion from cached content, and prompt-template file loading with `--prompt-template`/`--no-prompt-templates` CLI flags. The executable owns CLI11 parsing, TTY-based frontend selection, and the final composition that may depend on both runtime and Native TUI targets.

The build publishes `include` as the public surface and keeps `src` private. Legacy synchronous tools, Boost.JSON contracts, `util::Result`, and duplicate `src` contract headers have been removed.

### pi parity direction

The harness aims for idiomatic C++ parity with pi's module and contract semantics rather than mechanical TypeScript translation. Supported Capability claims, including Native TUI selection and interaction, are pinned to pi baseline `864b35c`. The current implemented boundary is described by this README, the public headers, and tests. Open product and scope decisions are indexed in the [pi C++ parity map](https://github.com/lanshengzhi/cpp-coding-harness/issues/2); approved work leaves that map and follows `/to-spec` → `/to-tickets` → `/implement`.

## CLI states

One-shot text output prints stable semantic event lines:

- `[model-request]`
- `[assistant] <text delta>`
- `[tool-call] <name>#<id>`
- `[tool-success] <id>`
- `[tool-error] <id>`
- `[error] <diagnostic>` — the final Assistant Message of the turn ended with stop reason `error`
- `[aborted] <diagnostic>` — the final Assistant Message of the turn ended with stop reason `aborted`
- `[completed]`

An accepted provider `error` or `aborted` outcome is presented exactly once (with secret redaction and bounded output applied to the diagnostic) and the prompt still completes successfully: one-shot runs exit 0 and the Native TUI remains usable for the next prompt. Prompt rejections before acceptance are reported on stderr as `loop failed: <message>` and return a non-zero exit status; they do not introduce a second text-event status model.

`--mode json` emits one compact JSON object per stdout line. The first record is the pi v3 session header; every later record is a directly serialized supported AgentSession event such as `agent_start`, `turn_start`, `message_start`, `message_update`, `tool_execution_start`, `tool_execution_update`, `tool_execution_end`, `turn_end`, and `agent_end`. Event payloads retain their message/tool structure after secret redaction and bounded-output transformation. JSON mode has no C++ schema/version envelope, sequence counter, content-status substitution, or `runtime_terminal` record. Prompt failures are reported on stderr with a non-zero exit. Frontend-only slash commands produce no post-header JSON record because they do not enter AgentSession.

`--mode rpc` is a narrow JSONL stdin/stdout command loop. It supports `prompt`, `get_state`, `get_last_assistant_text`, and `shutdown`; unsupported pi RPC commands return structured `success:false` responses. `get_state` reports the persisted transcript path as `sessionFile` and omits that key for in-memory sessions, matching pi's `RpcSessionState`. RPC mode emits no startup session header and interleaves command responses with direct AgentSession events on stdout. Prompt success is acknowledged after session preflight accepts the prompt and before its first event; preflight rejection returns `success:false`. Later execution success or failure never emits a second response or a `runtime_terminal` record. Startup/pre-session failures remain on stderr. The protocol is sequential only: no `abort`, `steer`, `follow_up`, session switching, fork/clone, compaction, or extension UI yet.

### Slash commands

Every text-capable frontend assembles this implemented baseline subset:

| Command | Behavior |
| --- | --- |
| `/help [command]` | List all effective registry names, or show detailed help for one name. Both `/help name` and `/help /name` are accepted. |
| `/commands` | Alias for `/help`. |
| `/session` | Show the current session id, session file (`In-memory` when running with `--no-session`), workspace, provider, model, and message count. |
| `/clear` | Clear the frontend terminal. Arguments produce `Usage: /clear`. |
| `/quit` | Request a clean frontend shutdown. |
| `/exit` | Alias for `/quit`. |

The Native TUI concretely extends its effective registry with `/settings` (the supported theme settings overlay) and `/hotkeys` (help for the exact effective binding registry). Instructional placeholders such as `/new` and `/resume` and other deferred pi actions are not registered or advertised.

Aliases are resolved by the command registry and appear in `/help`. Native TUI slash completion derives built-ins from that same effective registry and adds the loaded prompt templates and `/skill:<name>` invocations. Registry help does not mislabel those project resources as built-ins. Frontend commands are resolved before input reaches `AgentSession`; unmatched slash input continues through the cached skill → prompt-template interpretation path. Bare `/` and unmatched slash input pass through to the model unchanged.

Command presentation depends on the frontend:

| Input | Native TUI | Text one-shot | JSON mode | RPC `prompt` |
| --- | --- | --- | --- | --- |
| Help or session information | Render a local presentation entry without changing or persisting Agent Session history. | Display the command message without invoking the model. | Emit only the session header; frontend command outcomes are not AgentSession events. | Sent as ordinary prompt text; RPC built-ins are JSON commands, not text-frontend slash commands. |
| `/settings` or `/hotkeys` | Open the supported overlay without invoking the model. | Unmatched input follows ordinary prompt interpretation. | Unmatched input follows ordinary prompt interpretation. | Sent as ordinary prompt text. |
| Exact `/clear` | Clear through the Terminal seam without prompting the session. | Clear only when both streams are terminals; non-TTY output emits no ANSI. The command never invokes the model. | Emit only the session header and no ANSI bytes. | Sent as ordinary prompt text; no terminal-control bytes are emitted by the adapter. |
| `/quit` or `/exit` | Close the Agent Session and restore terminal state cleanly. | Display `Shutting down.` and exit successfully. | Emit only the session header, then exit successfully. | Sent as ordinary prompt text; use the RPC `shutdown` command to stop the loop. |

### Input prefixes (User Bash)

In the Native TUI on supported Linux/macOS, a direct focused-editor submission whose trimmed text begins with `!` is User Bash: the remainder runs as one shell command in the Session workspace and streams into a single `$ command` transcript block (ADR 0026). A `!` result is committed to Session history as a pi v3 `bashExecution` message and may enter later model context; a `!!` result is committed the same way but is excluded entirely from model conversion. User Bash is always available in the Native TUI and is independent of `--enable-bash`, which continues to authorize only the model-requested `bash` tool.

The parsed command is trimmed and may span multiple lines; a bare `!` or `!!` falls through to an ordinary Agent Prompt, and `!!!foo` is excluded User Bash running `!foo`. Only direct focused-editor submissions interpret the prefix: positional initial input, one-shot text, JSON, RPC, SDK, and Skill or Prompt Template expansions beginning with `!` remain ordinary prompt text, and the prefixes are never slash commands, autocomplete items, or hotkey actions. At most one User Bash runs at a time; it may overlap an Agent run, with completion committed after the whole run settles, and the effective interrupt binding cancels an active Agent run before an active User Bash. Execution uses the same effective `shellPath`/`shellCommandPrefix` configuration, filtered environment, and `/bin/bash` → PATH `bash` → `sh` (`-c`) resolution as the model tool, has no default timeout, starts every command from the canonical Session workspace, and cancels through process-tree termination. Output is ANSI-stripped, secret-redacted, and bounded to a 2,000-line/50 KiB tail, with truncated full output spilled to a unique owner-only temporary file.

In the one-shot, JSON, RPC, and SDK paths, leading `!` text remains ordinary prompt text.

## Embeddable C++ SDK

The project exposes an experimental same-process C++23 SDK surface for host applications that want to embed the agent loop without shelling out to `cpp_harness` or depending on CLI/RPC globals. The SDK is source-level only (not ABI-stable) and lives under `include/cch/coding_agent/Sdk.hpp`.

```cpp
#include <cch/coding_agent/Sdk.hpp>

using namespace cch;

// Default: create a persisted session under the Agent Config Directory,
// grouped by the canonical physical workspace.
coding_agent::CreateAgentSessionOptions opts;
opts.workspace = std::filesystem::current_path();
opts.provider_config = coding_agent::SdkProviderConfig{
    .provider = "openai-compatible",
    .model = "gpt-4o-mini",
    .base_url = "https://api.openai.com/v1",
    .api_key_env = {{"OPENAI_API_KEY"}},
};

auto result = coding_agent::create_agent_session(std::move(opts));
if (!result) {
    // handle creation error — check result.error().message
}

// Persisted creation and resume return the actual JSONL path through an
// optional contract.
if (result->session_path) {
    // use *result->session_path
}

auto& session = result->session;

// Subscribe to lifecycle events
int events = 0;
auto sub = session->subscribe(
    [&](const agent::AgentLifecycleEvent&) -> util::ExpectedVoid {
        ++events;
        return {};
    });

// From a coroutine running on the host's Asio executor, run a prompt.
// Progress arrives through the persistent subscription.
if (auto prompted = co_await session->prompt("hello"); !prompted) {
    // handle prompted.error()
}
auto last_text = session->last_assistant_text();
auto message_count = session->message_count();

// Copy one passive presentation snapshot. The nested AgentState is the
// authoritative live history, running/streaming/tool/model/thinking state,
// and diagnostics; metadata/topology/path describe the assembled session.
auto snapshot = session->snapshot();
for (const auto& message : snapshot.agent_state.messages) {
    // render the ordered Agent Message history
}

// Close (idempotent — repeated close is safe)
session->close();
```

Use the explicit in-memory target when the host needs normal Agent Session behavior without a transcript or session directory:

```cpp
coding_agent::CreateAgentSessionOptions opts;
opts.session_target = coding_agent::InMemorySessionTarget{};
opts.workspace = std::filesystem::current_path();
opts.provider_config = coding_agent::SdkProviderConfig{
    .provider = "openai-compatible",
    .model = "gpt-4o-mini",
    .base_url = "https://api.openai.com/v1",
    .api_key_env = {{"OPENAI_API_KEY"}},
};

auto result = coding_agent::create_agent_session(std::move(opts));
if (!result) {
    // handle creation error
}

// UUID metadata, events, prompts, tools, and live state remain available.
auto& session = result->session;
session->prompt_blocking("inspect the workspace");
auto count = session->message_count();

// Both optional path accessors are absent; no JSONL file is created.
if (result->session_path || session->session_path()) {
    // unexpected for InMemorySessionTarget
}
session->close();
```

**Supported SDK v1 behavior:**

- One passive `SessionTarget` variant: default persisted creation, `ExplicitNewSessionTarget`, `ExplicitResumeSessionTarget`, or `InMemorySessionTarget`. Default construction stores a workspace-keyed session beneath the Agent Config Directory; explicit paths are used exactly and may live elsewhere; in-memory creation never publishes a session directory or JSONL file.
- `CreateAgentSessionResult::session_path` and `AgentSession::session_path()` use `std::optional<std::filesystem::path>`. Persisted targets return the actual path; in-memory sessions return no value, never an empty-path sentinel.
- The old experimental `session_path` / `resume_path` option fields were intentionally removed without aliases or compatibility adapters. Explicit targets now look like `opts.session_target = coding_agent::ExplicitNewSessionTarget{"/tmp/my-session.jsonl"};` or `ExplicitResumeSessionTarget{"/tmp/my-session.jsonl"}`.
- Async-first `prompt()` — an awaitable, serial, single-prompt-at-a-time operation that runs on the host's Asio executor without a session-owned thread or nested prompt loop. Provider rejection before runtime transport, failures reported by the provider event sink, and persistence failures return an explicit `util::Error`; accepted provider `error` and `aborted` outcomes complete successfully and expose their final Assistant Message through ordinary lifecycle events and Live Session State.
- Synchronous `steer()` and `follow_up()` admission backed directly by the Agent-owned queues. `CreateAgentSessionOptions::max_queued_messages` and `max_queued_bytes` configure each queue, snapshots expose the effective bounds and pending values, and rejection occurs before mutation.
- Synchronous queue mode and clear operations preserve the pi-compatible steering-before-follow-up and one-at-a-time/all drain policies without a capability-advertisement contract.
- Synchronous `abort()` requests cancellation of the one active prompt through its prompt-scoped `std::stop_token`. Repeated calls and idle calls are no-ops. An accepted abort completes the ordinary assistant message, turn, and agent lifecycle with stop reason `aborted`; after the prompt awaitable quiesces, the same session accepts another prompt. Like other session operations, `abort()` is confined to the executor driving the prompt and is not an unrelated-thread synchronization API.
- Synchronous `close()` is a non-blocking, non-throwing, idempotent request. It rejects new prompts and subscriptions immediately, requests the active prompt's ordinary aborted lifecycle, and releases subscribers plus SDK-owned clients, tools, storage, and execution resources exactly once after active callbacks and operations quiesce. Host-provided execution environments are never cleaned up by session close.
- `prompt_blocking()` is a separately named convenience facade over the async path. It owns a temporary executor for the call and rejects same-session callback reentry that would recursively wait on itself.
- One persistent event-subscription path via move-only `agent::AgentEventSink`; prompt progress is not returned or delivered through per-prompt sinks.
- Host-provided chat clients and execution environments. Chat clients and custom tools passed by `unique_ptr` transfer ownership to the session; `shared_ptr` execution environments remain host-owned and are not cleaned up by session close.
- SDK convenience provider construction from `SdkProviderConfig` when no host client is supplied.
- Built-in tool selection with safe defaults: `read`, `write`, and `edit_file` by default; `bash` requires explicit opt-in.
- Custom tool registration via existing `agent::AsyncAgentTool` contracts; duplicate tool names fail creation.
- Programmatic skills and prompt templates; host resources take precedence over project-discovered duplicates.
- Per-prompt `PromptOptions::expand_prompt_templates` (default `true`); `false` bypasses skill and prompt-template expansion and sends raw text to the agent loop. Resulting message count and last assistant text are queried through `AgentSession` state accessors.
- `AgentSession::snapshot()` returns one independent passive `AgentSessionSnapshot`: an exact copy of authoritative
  `AgentState` plus Session metadata, reconstructed active-path topology, and an optional persisted path. Snapshot access
  is executor-confined like other Session state access, performs no callbacks, persistence, dispatch, or Agent reentry,
  and may be used by a lifecycle subscriber during an active run. A snapshot can be mutated freely without changing live
  or durable state.
- Optional project resource discovery (`.cpp-harness/skills/`, `.cpp-harness/prompts/`) behind explicit trust/resource controls.
- Optional absolute external `trust_store_path` for SDK project-resource trust decisions; workspace-local trust-store paths are rejected.
- Bounded resource diagnostics with stable codes returned as `CreateAgentSessionResult::diagnostics` values — no stdout/stderr output from the SDK path.
- Resolved session metadata on `CreateAgentSessionResult::metadata`, matching the created/resumed JSONL session header. Default creation uses one canonical physical workspace for execution, metadata, and automatic path encoding, so filesystem aliases share a session directory.

**Not supported in SDK v1:**

- ABI-stable binary distribution, plugin ABI, or package-manager integration.
- Full pi `AgentSessionRuntime` replacement APIs (`newSession`, `switchSession`, `fork`, `clone`, import/export).
- Public branch/tree navigation or SDK append support for non-linear session topologies (SDK v1 returns `unsupported_session_topology` for branched/compacted sessions).
- Concurrent prompts or unrelated-thread queue admission.
- Dynamic TypeScript/JavaScript extensions, extension UI, hot reload, MCP, or package installation.
- TUI run modes, themes, keybindings, widgets, OAuth/subscription providers, or model catalogs.

Frontend selection follows pi baseline `864b35c` after argument parsing and before Agent Session creation. `--mode rpc` selects RPC and `--mode json` selects JSON even with `--print`; otherwise `-p` / `--print` or either non-TTY stream selects one-shot text output. `--mode text` leaves that selection unchanged. On supported Linux/macOS, interactive stdin/stdout selects the Native TUI, and a positional prompt becomes its initial message. Other interactive platforms fail clearly; print, JSON, and RPC remain available. One-shot output requires positional, file, or piped input. Loaded skills are explicitly invocable with `/skill:<name> [additional instructions]`; that input expands to a regular user prompt containing the skill instructions. Project-local resources can be trusted for one run with `--approve` / `-a`, denied for one run with `--no-approve`, and project skills can be suppressed with `--no-skills`. RPC mode reads prompts from stdin commands rather than positional CLI arguments. `--resume <session.jsonl>` starts from the active tree path before persisting new messages through the current session store. `--session <path>` always creates a new file; use `--resume` to append.

## Skills

At startup, the CLI scans project-local `.cpp-harness/skills` for nested `SKILL.md` files only when the project resource load plan allows project skills. A skill file uses flat YAML frontmatter (`name`, `description`, optional `disable-model-invocation`) followed by markdown instructions. Valid skills are loaded into the session, diagnostics for malformed or duplicate skills print to stderr, and visible skills are injected into model context through the pi-shaped `<available_skills>` block.

Skills with `disable-model-invocation: true` are hidden from the model-visible list but can still be invoked explicitly with `/skill:<name> [additional instructions]` when the skill was loaded. Invocation uses the skill body cached at startup; edit/reload behavior during a running session, global `~/.cpp-harness/agent/skills`, and config-driven skill directories are deferred.

Explicit `--prompt-template` inputs may be `.md` files or directories containing loadable `.md` files. A missing, malformed, or unsupported explicit file—or an explicit directory with no loadable templates—aborts creation before the session is published. Malformed auto-discovered project templates are skipped instead and produce bounded diagnostics.

## Native TUI themes

The Native TUI theme catalog always includes built-in `dark` and `light` themes. One startup discovery pass reads global JSON themes from `<agent config directory>/themes`, accepts project theme documents only after Project Trust admits `.cpp-harness/themes`, and accepts explicitly supplied JSON files or directories from the assembling frontend. Same-name precedence is explicit resources, trusted project resources, global resources, then built-ins; the first same-tier resource in deterministic path/input order wins. Explicit active selection outranks the `theme` in User Settings. With neither selection, terminal appearance chooses the built-in light or dark palette directly, even when a same-name custom resource exists.

A malformed or unavailable explicit theme resource or explicit active name fails catalog assembly. Malformed auto-discovered resources are skipped with redact-before-bound diagnostics (at most 64 diagnostics and 1024 bytes per message/path). The Theme settings overlay lists the effective catalog, commits a selected name through a narrow settings callback, replaces the live palette, and invalidates all TUI presentation. Theme discovery is not watched or reloaded during the session. These resources are loaded by the production Native TUI without adding an explicit TUI flag or mode.

## Native TUI keybindings

One startup pass reads `keybindings.json` only from this product's Agent Config Directory. The baseline-compatible format accepts one key string, multiple alternatives, or an empty array per namespaced action. Valid user entries replace only that action's defaults; context-local default reuse remains intact, while user/user conflicts are diagnosed and resolved by contextual candidate order. Invalid keys, unknown IDs, and known but unassembled application IDs are skipped with bounded, redacted diagnostics and never create inert bindings.

The exact immutable effective registry drives component dispatch, hotkey help, and rendered key hints. The Native TUI registers `app.message.followUp` (`alt+enter`) and `app.message.dequeue` (`alt+up`) only because both operations are assembled end to end. Idle Enter or Alt+Enter starts an ordinary prompt; Enter during active work admits steering input, while Alt+Enter admits follow-up input. `shift+enter` and `ctrl+j` remain multiline editor actions. Accepted input leaves the editor and appears in the pending queue display; rejected input is restored unchanged with a bounded, redacted diagnostic. Dequeue restores steering messages, then follow-up messages, then existing editor text, separated by blank lines. Interrupt restores pending text before requesting abort. Linux/macOS job-control defaults and native Windows unavailability are explicit when an application action is concretely registered. See [Native TUI keybindings](docs/keybindings.md) for the grammar, implemented IDs, precedence, diagnostics, platform behavior, and pi baseline sources. There is no explicit TUI mode and no hot reload.

## Project trust and resource controls

Project trust controls whether project-authored `.cpp-harness` resources are loaded at startup. It is an input-loading guard, not a sandbox: it does not restrict built-in tools, model output, prompt injection from files you choose to read, shell commands enabled with `--enable-bash`, or transcript content already present in a resumed session.

Protected project markers include `.cpp-harness/settings.json`, `.cpp-harness/skills`, `.cpp-harness/prompts`, `.cpp-harness/themes`, `.cpp-harness/extensions`, `.cpp-harness/packages`, `.cpp-harness/SYSTEM.md`, and `.cpp-harness/APPEND_SYSTEM.md`. A bare `.cpp-harness` directory and `.cpp-harness/sessions` do not require trust. In non-interactive startup, the default `ask` policy acts as untrusted unless a saved trust decision or same-run override exists. Trust decisions are read from user-controlled `~/.cpp-harness/agent/trust.json` with nearest-parent inheritance.

User settings in `~/.cpp-harness/agent/settings.json` can set provider defaults (`provider`, `model`, `base_url`, `api_key_env`, `auth`), a Native TUI `theme` name, `default_project_trust` to `ask`, `always`, or `never`, and `project_resources.skills` / `project_resources.themes` to `auto`, `on`, or `off`. Optional compatible `shellPath` and `shellCommandPrefix` strings configure local Shell launches from the one startup settings snapshot. A leading `~` in `shellPath` expands to the user's home directory; a non-empty prefix is joined to each script with one newline but is not added to tool-visible command text or Session history. The optional `sessionDir` string (pi vocabulary) is a CLI session-storage preference below `--session-dir` and `CCH_CODING_AGENT_SESSION_DIR`; it is not a provider setting and SDK default persistence never reads it. `off` always skips project skills. `--approve` / `-a` and `--no-approve` are one-run trust overrides; they do not persist decisions. `--no-skills` disables project-local skills for the run even when the project is trusted.

## Tools

The built-in tools are:

- `read_file`: read a text file inside the workspace, with optional line offset/limit. Appends a continuation hint when output is truncated.
- `write_file`: create or overwrite a file inside the workspace; creates parent directories implicitly.
- `edit_file`: perform one or more exact replacements via an `edits[]` array of `{oldText, newText}` pairs; zero or multiple matches per edit are rejected. Multiple disjoint edits in one call are applied together.
- `bash`: run a shell command inside the workspace only when `--enable-bash` is passed. Accepts a timeout in seconds and strips ANSI escape sequences from output. On supported Unix platforms, local execution resolves an expanded configured `shellPath`, otherwise `/bin/bash`, PATH `bash`, then PATH `sh`, and invokes it with `-c` rather than as a login shell. A stale configured path fails only the attempted execution; it does not prevent Session startup.

The registry owns tool capabilities directly. In default sequential execution, every call starts its Tool Execution lifecycle before the executor parses and clones the raw arguments, applies the recorded pi-baseline recursive coercion profile, and validates the tool's JSON Schema before policy hooks or capability invocation. The executable profile covers primitive and union types, nested objects and arrays (including tuple items and dynamic properties), value and collection constraints, composition, and baseline-recognized formats. Unknown formats and members remain annotations unless a required vocabulary makes support mandatory. Malformed JSON, unknown tools, invalid or unsupported dialects and vocabularies, unresolved references, unsupported executable constructs, and schema-invalid values become bounded error Tool Call Outcomes for only their calls; annotations and extension members do not disable recognized assertions. Parser excerpts in malformed-argument diagnostics are secret-redacted before UTF-8-safe truncation, so the same safe diagnostic flows through tool lifecycle events, model context, and Session persistence.

Tools default to `Exclusive`; bounded parallel execution requires both an explicit run policy and a tool adapter that declares `ParallelSafe`. The built-in tools remain `Exclusive` because their shared execution environment has no public concurrent-use contract. A tool may override `execute_with_updates()` to publish cumulative partial tool execution results through the move-only update sink; those updates retain the Tool Call ID through Agent lifecycle, JSON/RPC, and Native TUI presentation, late updates after execution settlement are ignored, and ordinary tools continue to implement `execute()`. The active prompt's `std::stop_token` is mandatory at the tool and filesystem capability seams. Sequential execution observes cancellation before starting each call and after the current hook/tool outcome is finalized. Bounded-parallel workers observe it before claiming and before invoking queued capabilities; already-running calls quiesce, while claimed-but-unstarted calls receive balanced `Operation aborted` outcomes without capability invocation.

File tools deliberately share one execution environment capability, which owns workspace containment, path validation, atomic writes, process execution, timeout handling, and output limiting. Awaitable filesystem operations observe cancellation immediately before and after their synchronous local filesystem boundary; a completed mutation is not rolled back when cancellation wins the post-operation check. Best-effort `cleanup()` is deliberately the sole no-token operation. Shell execution carries the same token through `ExecOptions` and `ProcessRequest`; on supported platforms cancellation terminates the active process group and reaps the shell before returning `Aborted`. File tools reject workspace escapes, symlink escapes, directory/file mismatches, and missing parents unless creation is explicitly requested. `bash` receives a sanitized environment that omits API-key, token, secret, password, and OpenAI-looking variables.

## Sessions and safety

Sessions are JSONL:

1. a pi-style v3 `session` header with session/workspace/provider/model metadata (legacy v2 headers remain readable),
2. append-only typed `message` entries containing redacted user, assistant, and tool-result messages,
3. write support for v3 tree metadata entries (`model_change`, `thinking_level_change`, `active_tools_change`, `custom`, `custom_message`, `label`, `compaction`, `branch_summary`, `session_info`) and extended runtime messages (`BashExecutionMessage`, `CustomMessage`, `BranchSummaryMessage`, `CompactionSummaryMessage`),
4. safely ignored unknown future entry types.

Each completed user, assistant, or tool-result message enters the stateful Agent's live history before weak subscribers run and is appended after observer delivery. Subscriber failures or exceptions become bounded Agent diagnostics and deactivate the faulty observer; they do not stop later observers, veto Agent progress, or prevent persistence. A persistence failure remains a strong commitment failure: it stops the current run without rolling back live history or closing the session. Later in-process prompts use retained live state, while reopening reconstructs only entries that reached durable storage. Current resume uses the v3 session tree to rebuild the active path, including persisted leaf selection and compaction summaries, through the session resume module. Exact unredacted replay is intentionally out of scope. Session files are still sensitive: they can contain source text, command output, workspace paths, and provider/model metadata.

The workspace guard is not a sandbox. Prompts, file contents, and command outputs can be sent to the configured provider. Run this harness inside a VM/container if you need a real containment boundary.

## Executable specs

Useful default validation slices:

```bash
./build/cpp_harness_tests "[architecture]"
./build/cpp_harness_tests "[ai][u2]"
./build/cpp_harness_tests "[ai][provider]"
./build/cpp_harness_tests "[agent][stateful]"
./build/cpp_harness_tests "[agent][async]"
./build/cpp_harness_tests "[tools][async]"
./build/cpp_harness_tests "[harness][session]"
./build/cpp_harness_tests "[sdk][live-state]"
./build/cpp_harness_tests "[sdk][snapshot]"
./build/cpp_harness_tests "[sdk][incremental-persistence]"
./build/cpp_harness_tests "[coding-agent][json-events]"
./build/cpp_harness_tests "[coding-agent][runtime][rpc]"
./build/cpp_harness_tests "[cli]"
```

These cover:

- public headers compile from the include contract surface without `src`, legacy sync contracts, Boost.JSON, or raw Glaze generic values in domain contracts;
- stateful Agent prompts retain live history, expose passive snapshots, reject overlapping runs, and notify weak move-only observers in lifecycle order;
- fake model/tool loops route through provider-neutral value contracts and owned tool capabilities;
- SDK abort propagates one prompt-scoped stop token through Agent policies and provider transport, completes the ordinary aborted lifecycle, and leaves the session reusable;
- move-only event sinks can capture unique state and propagate errors;
- provider-specific OpenAI/SSE/Glaze wire mapping stays isolated from the agent loop;
- JSONL resume reconstructs typed message ordering for the next request;
- AgentSession tests protect live-state-before-subscriber-before-persistence ordering, passive fresh/in-memory/active/resumed snapshots, and failure recovery;
- JSON/RPC transcript tests protect direct events, prompt preflight acknowledgement, response correlation, and terminal-record absence;
- CLI fake-provider smoke tests demonstrate the current event/subscriber path without compatibility-only flags.

## Deferred

Not included yet: Native Windows TUI support; extensions; packages; global/config-driven skill directories; live skill reload; OAuth; full pi RPC command parity; MCP integration; permission prompts; native Windows shell process-tree termination semantics; subagents; C++26 reflection-generated schema; `std::execution` senders/receivers; ABI-stable binary distribution; or OS-level sandboxing.

An experimental same-process embeddable C++ SDK surface is available (see Embeddable C++ SDK section above). Full pi SDK parity (session replacement runtime, concurrent prompts, compaction, ABI stability) is deferred.

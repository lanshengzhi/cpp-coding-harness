# C++ Coding Harness

A small experimental C++23 coding-agent harness built around anti-fragile architecture rules:

1. **Data is passive value state**: public contracts are aggregate-friendly structs, `std::variant` alternatives, `std::expected` failures, and a project `JsonValue` for unstructured JSON facts.
2. **Capabilities cross physical seams**: chat clients, stream transports, execution environments, session stores, and tools are replaceable interfaces or dependency-heavy concrete implementations hidden behind headers.
3. **Events are weak connections**: agent/provider event sinks use move-only callback semantics so subscribers can own unique state without forcing `std::shared_ptr` or copyability.
4. **Generic machinery stays local**: Glaze DTOs, schema conversion, visitors, parsing helpers, and future reflection-friendly machinery live in serialization/implementation layers rather than domain-facing APIs.

The harness loop mirrors the core pi-style flow:

1. accept a prompt from the Native TUI or the one-shot print CLI,
2. send ordered messages plus JSON Schema tool definitions to a provider chat API,
3. execute local tools requested through `tool_calls`,
4. append tool-result messages with matching call IDs,
5. repeat until the assistant stops,
6. persist the redacted typed transcript as JSONL.

This is a learning and experimentation harness, not a production sandbox.

## Build

The project is CMake-based and requires a C++23-capable compiler. CMake 3.25 or newer is expected.

- Glaze is used only at typed JSON serialization/deserialization boundaries.
- utf8proc provides versioned Unicode grapheme segmentation and width properties inside the private TUI implementation.
- MD4C provides tolerant Markdown parsing behind the public TUI component boundary.
- Pinned stb image decode/resize/write headers are vendored under `third_party/stb` and compiled only by the private initial-image processor; their upstream license notices remain in each header. libwebp supplies private WebP validation/decoding for the same processor.
- Boost.Beast/Asio + OpenSSL provide the HTTPS transport implementation.
- Boost.Process is used behind the process-execution capability boundary.
- CLI11 is declared in `vcpkg.json`; tests use the repository's tiny Catch-compatible test header under `third_party/catch2`, so no external Catch2 dependency is required.

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

### Fast development preset (Ninja + ccache)

For the fastest edit-build-test loop, use the checked-in `dev-fast` preset family. It configures with the Ninja generator, enables and *requires* ccache (configure fails with a clear message when ccache is not installed), and defaults to four parallel jobs. It keeps its own build tree under `build/dev-fast` and leaves the baseline presets above unchanged. This is the project's explicit fast-development path (see `docs/build-performance-plan.md`, Stage 2); on this host a warm ccache clean rebuild is about two orders of magnitude faster than a cold build.

```bash
cmake --preset dev-fast
cmake --build --preset dev-fast
ctest --preset dev-fast
```

A Release form is available as `dev-fast-release` (build tree `build/dev-fast-release`):

```bash
cmake --preset dev-fast-release
cmake --build --preset dev-fast-release
ctest --preset dev-fast-release
```

Both presets default to four build jobs, matching the measured host. On a high-memory host, override the job count explicitly on the command line without touching the preset:

```bash
cmake --build --preset dev-fast --parallel 8
```

### Using system packages

If you prefer system-installed dependencies, install Boost, OpenSSL, Glaze, MD4C, libwebp, CLI11, and utf8proc yourself, then use the system preset:

```bash
cmake --preset system
cmake --build --preset system
ctest --preset system
```

Run the binary against a configured provider (deterministic provider behavior is exercised through the test suite). With no session-family flag (`--session`, `--resume`, `--continue`, `--fork`, `--session-id`, `--no-session`), each run persists a new session under the workspace-keyed user-level default (see [Session storage](#session-storage)):

```bash
./build/cpp_harness --print "hello"
printf 'hello from stdin' | ./build/cpp_harness --print
./build/cpp_harness @screenshot.png "describe this image"
./build/cpp_harness @notes.txt @diagram.webp "compare these files"
```

Positional `@path` arguments are content-sniffed rather than classified by extension. PNG, JPEG, GIF, and WebP image bytes contribute an absolute `<file name="…"></file>` reference to the initial text and an ordinary `ai::ImageContent` block after that complete text block. Non-image files contribute their text inside the same absolute-path wrapper. A lone image still creates the wrapper text and therefore uses the existing text-plus-images Agent Session operation; there is no image-only prompt API.

Provider-bound images are preserved unchanged while dimensions are at most 2000×2000 and the base64 payload is strictly below 4.5 MiB. Larger decodable PNG, JPEG, GIF, and WebP inputs are aspect-ratio resized and carry the baseline coordinate-mapping hint in their `<file>` reference; an input that cannot be safely reduced is represented by an omission note rather than malformed image content. Empty files are skipped, while a missing or unreadable initial file fails before Session Publication.

Pass `--no-session` to run any frontend in memory with no transcript left behind:

```bash
./build/cpp_harness --no-session
./build/cpp_harness --print --no-session "hello"
```

Explicit create and resume targets remain available:

```bash
./build/cpp_harness --session /tmp/cpp-session.jsonl "hello"
./build/cpp_harness --continue "continue"
./build/cpp_harness --resume
```

Redirect automatic storage for one run or for all runs (highest precedence first: `--session-dir`, `PI_CODING_AGENT_SESSION_DIR`, `sessionDir` in `~/.pi/agent/settings.json`):

```bash
./build/cpp_harness --session-dir /data/sessions --print "hello"
PI_CODING_AGENT_SESSION_DIR=/data/sessions ./build/cpp_harness --print "hello"
```

The coroutine/Glaze/event stack is the only active stack. Legacy compatibility flags such as `--async` are intentionally absent.

Real-provider mode resolves the model through the composed `ModelRuntime` catalog (`~/.pi/agent/models.json` overlaid on the built-in providers):

```bash
export KIMI_API_KEY=...
./build/cpp_harness --model kimi-for-coding "summarize README.md"
```

Model selection follows pi: `--model <pattern>` (with optional `--provider <name>`, or `provider/model`), then a resume's stored `model_change`, then settings `defaultProvider`/`defaultModel`, then the runtime default. `--models <patterns>` limits cycling. `--api-key <key>` installs an in-memory, never-persisted runtime API key override and requires an explicit model (`--model`, `--provider/--model`, or `--models`).

Request authentication resolves immediately before each request through pi's four-level chain: runtime API key override (`ModelRuntime::set_runtime_api_key`, CLI `--api-key`) → stored `auth.json` credential → provider environment variables → `models.json` configured `apiKey`. The removed `--base-url`, `--api-key-env`, and `--auth` flags fail loudly; custom endpoints become config-only providers in `models.json`. Stored OAuth credentials are refreshed by Providers that support OAuth. Login is explicit through an injected `AuthInteraction` (Codex PKCE callback-server browser flow, Kimi RFC 8628 device flow); the Native TUI owns the login presentation per ADR 0036.

### Agent config directory

User-level state lives directly in pi's agent config directory `~/.pi/agent/`: `auth.json` (credentials shared with pi), `settings.json` (pi two-scope user settings: `defaultProvider`/`defaultModel`/`defaultThinkingLevel`/`enabledModels`/`sessionDir`/`defaultProjectTrust`/`shellPath`/`shellCommandPrefix`/`theme`), `models.json` (custom providers and model configuration; never written by the runtime), `keybindings.json` (Native TUI bindings), `trust.json` (persisted project trust decisions), `themes/` (global Native TUI themes), and `sessions/<encoded-workspace>/` (default persisted Agent Session histories). Set `PI_CODING_AGENT_DIR` to override the location. These paths are centralized in `include/cch/coding_agent/AgentConfigDir.hpp`; there is no fallback read or migration from the former harness-private root.

### Session storage

Without a session-family flag, the CLI persists each new session under `<agent config directory>/sessions/<encoded workspace>/<UTC timestamp>_<session id>.jsonl`, mirroring pi's layout. The workspace key is pi's readable encoding of the canonical physical workspace path — the process working directory (the C++-only `--workspace` flag was deleted under ADR 0036; workspace containment is the internal `workspace := cwd` seam) — and symbolic-link aliases of one workspace share one session directory. The filename UUID matches the Session ID stored in the session header and reported by runtime state.

The session-family flags follow pi's semantics: `--session PATH` opens or creates at the exact path (or resolves a session id — exact then prefix, local then global; a session found in another project prompts to fork it into the current directory) and may live outside the default root; automatic-directory overrides never rewrite it. `--resume` opens the startup-TUI session picker (pi `selectSession`; the picked session resumes, cancel prints "No session selected" and exits 0). `--continue` resumes the most recently modified session in the session directory (cwd-filtered only under a custom `--session-dir`) or creates a new one. `--fork PATH|ID` copies a session's full history into a new session (pi conflict checks; the target id from `--session-id` must be free locally). `--session-id ID` validates the id, resumes an existing local session with that exact id, or warns and creates one. `--name NAME` appends a sanitized `session_info` entry (non-empty after trimming). Old project-local `.cpp-harness/sessions/` files are neither scanned nor migrated; pass such a file explicitly through `--session` if it is still needed. A missing or unwritable default store fails before any model work with the attempted path and reason — the harness never falls back to a workspace-local or temporary transcript. On POSIX, harness-created session directories are owner-only (`0700`) and new session files are `0600`.

Automatic-directory overrides replace the whole automatic directory for default creation: the session file lands directly in the override directory without a workspace-key component (pi: `--session-dir`). The precedence is exactly `--session-dir`, then `PI_CODING_AGENT_SESSION_DIR`, then `sessionDir` in `~/.pi/agent/settings.json`, then the workspace-keyed default. Absolute values stay absolute, a leading `~` expands to the home directory, and relative values resolve against the final canonical workspace rather than the launch directory. An override directory that does not exist is created owner-only; an existing override directory keeps its mode while files inside remain `0600`. These CLI-only inputs never affect default persistence.

`--no-session` selects an explicit in-memory Agent Session in every frontend (Native TUI and print mode): provider, tools, events, live state, errors, and shutdown behave normally, but no sessions root, workspace-local session directory, or transcript file is created — including after failures. It short-circuits silently ahead of `--session`, `--resume`, and `--continue` (pi precedence; no conflict error), while `--fork` combined with `--no-session` is rejected like pi. In-memory operation happens only when explicitly requested; storage unavailability is never silently treated as `--no-session`. `/session` prints the persisted file path, or `File: In-memory` for these runs.

### Auth file

API keys can be stored in `~/.pi/agent/auth.json` (shared with pi). A stored credential for a provider is resolved at request time and takes precedence over that provider's environment variables and its `models.json` configured key:

```json
{
  "kimi-coding": { "type": "api_key", "key": "..." },
  "deepseek": { "type": "api_key", "key": "..." }
}
```

```bash
# Uses the kimi-coding auth entry automatically; no env var needed
./build/cpp_harness --model kimi-for-coding "summarize README.md"
```

Credentials never enter settings.json; `models.json` `apiKey` carries only configured keys for config-only providers. The `--auth` flag and the settings `auth` field are removed.

### Kimi Code

Kimi Code is a built-in provider (`kimi-coding`, model `kimi-for-coding`) that resolves its API key from `KIMI_API_KEY` (or a stored `auth.json` entry, or a `--api-key` runtime override):

```bash
export KIMI_API_KEY=...
./build/cpp_harness --model kimi-for-coding "summarize README.md"
```

Kimi's `ANTHROPIC_BASE_URL` / `ANTHROPIC_API_KEY` examples are for Anthropic-shaped Claude Code clients. This harness does not read those variables or implement an Anthropic provider.

Live Kimi usage sends prompts, file contents read by tools, and tool outputs to the configured provider. JSONL session redaction is a persistence boundary, not a guarantee that terminal output, CI logs, provider diagnostics, or provider-bound tool results are redacted. Do not paste raw credentials into prompts, files, or tool-visible content.

Resuming reconstructs the redacted active session path, including persisted leaf and compaction context, and re-resolves the stored `model_change {provider, modelId}` against the live `ModelRuntime` catalog. When you omit `--model`/`--provider`, the fallback chain is: the resumed session's `model_change` → settings `defaultProvider`/`defaultModel` → the runtime default. A stored model that no longer resolves surfaces a diagnostic without silent substitution. Explicit CLI flags always win.

Troubleshooting:

| Symptom | Check |
| --- | --- |
| `No API key found for … Use /login …` / `Authentication failed for … Run '/login X'` | pi's re-auth guidance: the prompt preflight (or a request-time auth terminal) found no usable credential for the resolved model's provider. Export `KIMI_API_KEY`, add a `~/.pi/agent/auth.json` entry, or pass `--api-key`; for OAuth providers run `/login X` to re-authenticate. |
| `Provider is not configured` | The provider has no stored credential, ambient environment variable, `--api-key` override, or `models.json` configured key. Export `KIMI_API_KEY`, add a `~/.pi/agent/auth.json` entry, or pass `--api-key`. |
| Authentication or authorization failure | Confirm the key is valid for Kimi Code and the provider resolves to the Kimi endpoint. |
| Invalid model | Use `--model kimi-for-coding`, or configure a custom model in `models.json`. |
| Rate limit or quota error | Retry later or check Kimi Code subscription/entitlement and quota. |
| Provider or transport error | Re-run with harmless prompts and inspect diagnostics without printing secrets. |

Optional live smoke validation is manual and never part of default `ctest`:

```bash
CCH_LIVE_KIMI=1 KIMI_API_KEY=... scripts/kimi_live_smoke.sh
```

The smoke script requires explicit opt-in, uses a throwaway workspace/session, does not enable bash, and consumes real network/quota.

## Architecture boundaries

The code is split into value contracts, capability seams, implementation adapters, and package-style CMake targets:

- **`include/cch/`** — passive domain contracts and abstract capability seams. Public headers do not include Glaze, wire transport, or disk loader/formatting details.
- **`src/`** — implementation adapters kept private via CMake `PRIVATE` include paths: Glaze JSON/DTO serialization (`src/util/Json.hpp`, `src/ai/glaze/`), provider wire transport (`BoostBeastStreamTransport`, `SseParser`), and coding-agent loaders/formatters (`SkillLoader`, `PromptTemplateLoader`, `SkillFormatting`).

Package targets and responsibilities:

- `cch_util` (`include/cch/util`, `src/util`): project error/expected contracts, move-only callback vocabulary, passive `JsonValue`, the Glaze-backed JSON adapter in `src/util/Json.hpp`, and async process execution.
- `cch_tui` (`include/cch/tui`, `src/tui`): reusable source-level terminal UI contracts, a width-bounded structured Component seam, TUI root, semantic key and bracketed-paste input, a Unicode Editor with caller-supplied autocomplete and generic injected styling, Text, injected-style Markdown with optional syntax highlighting, filterable Select List and Settings List interactions, Loader and exactly-once Cancellable Loader lifecycles, structured inline Image placement with bounded fallback and private Kitty/iTerm2 protocol fixtures, a deterministic Virtual Terminal, and the transactional Linux/macOS Process Terminal used by the production Native TUI. The package depends only on project utility contracts, exposes no third-party types, has no coding-agent dependency, and makes no ABI-stability promise.
- `cch_coding_agent_tui` (`src/coding_agent/tui`): the physically separate coding-agent Native TUI presentation layer, re-composed as the pi interactive-mode subset (ADR 0036): the message/execution pipeline, selectors, login/trust dialogs, footer/status, and editor chrome assembled over the `cch_tui` toolkit, with pi's 42-action `app.*` catalog and pi-format themes. Known application actions are registered only when a frontend concretely assembles them, so deferred actions never become no-op bindings. Discovery is a one-shot startup operation with no generalized hot reload. This target remains independent of the Agent Session runtime.
- `cch_coding_agent_interactive` (`src/coding_agent/tui/InteractiveMode.*`): the private Native TUI composition that connects a real Agent Session to the reusable TUI, Editor, themes, and effective keybindings. It renders the initial Agent Session snapshot before focusing input; streams the pi message/execution pipeline (assistant and user messages with OSC 133 zones, tool and bash execution, summary messages); presents selectors and the login/trust dialogs as overlays; shows the footer/status surface; dispatches pi's slash chain before prompt interpretation with autocomplete from the same catalog; submits unmatched idle input asynchronously; routes active-run Enter and Alt+Enter directly to Agent Session steering and follow-up admission; visibly presents Agent-owned pending queues and restores them to the editor through the baseline dequeue action; routes the effective interrupt binding through pi's app.interrupt precedence (active Agent run, then running User Bash, then key-time pending User Bash submission); waits for abort quiescence before accepting another prompt or restoring the terminal; and restores Virtual or Process Terminals on exit. When an asynchronous clipboard reader is injected, its paste action writes sniffed PNG/JPEG/GIF/WebP bytes to a unique `pi-clipboard-<UUID>.<ext>` file under the OS temporary directory and inserts only that path at the editor cursor, falling back silently to clipboard text. The paste handler intentionally does not delete successful files or create attachment preview/removal state. The executable selects this composition for supported interactive terminals and keeps the runtime library and print-mode paths independent of TUI dependencies.
- `cch_ai` (`include/cch/ai`, `src/ai`): the complete credential-free Model value (including null-aware thinking mappings and the two-field Anthropic Messages compat), passive message/content/tool/context contracts, provider-neutral stream events, the Models/Provider runtime with request-time authentication, scripted fake Provider, and prompt cancellation propagation through Provider transport; SSE and Glaze provider mapping live under `src/ai/`.
- `cch_coding_agent_core` (`include/cch/coding_agent`, `src/coding_agent`): the ModelRuntime closure below the agent package — Agent Config Directory path resolution (`AgentConfigDir`), shared-file auth storage (`AuthStorage`) and the runtime API key overlay, `models.json` config (`ModelConfig`), built-in/config provider composition (`ProviderComposer`), and the sole public model/auth runtime seam (`ModelRuntime`, held as `std::shared_ptr` and injected into the stateful Agent and `CreateAgentSessionOptions`).
- `cch_agent` (`include/cch/agent`, `src/agent`): public stateful `Agent` ownership of live message history, model/thinking/tool state, weak move-only subscriptions with bounded diagnostics, passive state snapshots, one active run, and the strong per-run commitment seam used by Agent Session persistence. The Agent issues every turn through the injected `ModelRuntime::streamSimple` surface (the sole injectable seam per parity record #326), forwarding the session id, the default `"short"` cache retention, and the prompt cancellation signal per turn. The package also owns async tool registration, private Tool Argument Contract preparation, expected-style tool execution, pi-ordered prepare/stop/steering/follow-up policy seams, and sequential/bounded-parallel tool execution policy.
- `cch_harness` (`include/cch/harness`, `src/harness`): pi-shaped filesystem and shell execution capability contracts (`FileSystem`/`Shell`), local implementation with workspace containment, symlink safety, atomic writes, split-stream process execution, secret environment filtering, and JSONL/in-memory Session Store implementations.
- `cch_tools` (`include/cch/tools`, `src/tools`): built-in read/write/edit/bash tool factories bridging agent tool contracts to harness capabilities.
- `cch_coding_agent_runtime` (`src/coding_agent/runtime/`, `include/cch/coding_agent/`, `src/coding_agent/`, and private print-mode adapters in `src/cli/`): SessionFactory-authoritative session assembly (including the two-scope `settings.json` contract), the two-scope user settings manager (`SettingsManager`), session lifecycle, composition of the stateful Agent with persistence and resources, print-mode output (final assistant text only), pi `buildSystemPrompt` construction with the cch identity, strong incremental message persistence after Agent state and weak observers advance, project trust/resource controls (`ProjectTrust`, the pi `DefaultResourceLoader` subset over `.pi/` project markers), skill/template discovery and formatting, `/skill:name` expansion reading the file at invocation time, and prompt-template file loading with `--prompt-template`/`--no-prompt-templates` CLI flags.
- `cch_cli` (`src/cli`, `src/coding_agent/runtime/AsyncCliRuntime.*`): the single authoritative CMake owner of the CLI/runtime composition — CLI parsing, TTY-based frontend selection, list-models, the startup TUI, and the async CLI runtime — compiled once per configuration and linked into both the executable and the tests. It composes the private interactive Native TUI and CLI11 without adding a TUI dependency to the non-interactive runtime target. The `cpp_harness` executable owns only `src/main.cpp` and the final composition that may depend on both runtime and Native TUI targets.

The build publishes `include` as the public surface and keeps `src` private. Legacy synchronous tools, Boost.JSON contracts, `util::Result`, and duplicate `src` contract headers have been removed.

### pi parity direction

The harness aims for idiomatic C++ parity with pi's module and contract semantics rather than mechanical TypeScript translation. The pi-ai surface (Model/Models/ModelRuntime, authentication, and the three adapters), the pi-tui toolkit surface, and the pi-coding-agent application layer are pinned to pi baseline `83114817c68f5413e4d7ba6d7003ddc511cd31d2` (ADR 0029–0036). The current implemented boundary is described by this README, the public headers, and tests; where the README describes the pi-aligned surface decided by the pi-coding-agent phase, ADR 0036 is the authority and the code lands through `/to-spec` → `/to-tickets` → `/implement`. Open product and scope decisions are indexed in the [pi C++ parity map](https://github.com/lanshengzhi/cpp-coding-harness/issues/2); approved work leaves that map and follows `/to-spec` → `/to-tickets` → `/implement`.

## CLI states

One-shot text output follows pi's print mode (ADR 0036): it subscribes to nothing and prints only the final assistant message's `text` content blocks to stdout. A terminal `error` or `aborted` outcome prints the `errorMessage` (or `Request <stopReason>`) to stderr and exits 1 (pi `print-mode.ts`); success exits 0. Piped stdin, `@file` text, and the first positional merge into the initial prompt in pi's order with no separator; remaining positionals prompt sequentially and the output is the last response. Running with no prompt prints nothing and exits 0. Prompt rejections before acceptance are reported on stderr as `loop failed: <message>` with a non-zero exit status. SIGTERM/SIGHUP dispose the session and exit 143/129 (SIGHUP only outside Windows).

`--mode text` is the pi-default spelling and leaves TTY-based frontend selection unchanged; `--mode json` and `--mode rpc` are rejected with an explicit error — the JSON event stream and the JSONL RPC loop are removed with no placeholder surface (charted ruling, ADR 0036).

### Slash commands

The Native TUI dispatches pi's slash chain before prompt interpretation (ADR 0036). The Supported set is pi's builtin catalog scoped to the phase's capabilities:

| Command | Behavior |
| --- | --- |
| `/settings` | Open the settings selector (the #327 settings subset; the theme submenu per ADR 0036). |
| `/model [search]` | Open the model selector, or apply a model pattern. |
| `/scoped-models` | Open the scoped-models selector (enable/disable/reorder for cycling). |
| `/copy` | Copy the last agent message to the clipboard. |
| `/name <name>` | Name the session (`Usage: /name <name>`). |
| `/session` | Show session info and stats. |
| `/hotkeys` | Show help for the exact effective binding registry (assembled subset only). |
| `/fork` | Open the user-message selector to fork the session. |
| `/tree` | Open the session tree. |
| `/trust` | Open the project trust decision UI. |
| `/login [provider]` | Run the provider's OAuth or API-key login flow. |
| `/logout` | Open the OAuth logout selector. |
| `/new` | Start a new session. |
| `/compact [instructions]` | Compact the session, optionally with custom instructions. |
| `/resume` | Open the session selector. |
| `/reload` | Re-read User Settings, keybindings, skills, prompt templates, themes, and context files; refuses while streaming or compacting. |
| `/quit` | Request a clean frontend shutdown. |

`/export` `/import` `/share` `/changelog` `/debug`, the easter eggs, and `/clone` are Deferred with no placeholder surface: they are absent from autocomplete, and typed text passes through as an ordinary Agent Prompt (pi behavior for unrecognized slash text). `/clear` is not a command — it is the `app.clear` keybinding; help lives in the CLI `--help` surface. Autocomplete lists the Supported builtins (names/descriptions/argument hints), the loaded prompt templates, and `/skill:<name>` invocations while the Skill Commands setting is enabled. Print mode has no slash commands.

### Input prefixes (User Bash)

In the Native TUI on supported Linux/macOS, a direct focused-editor submission whose trimmed text begins with `!` is User Bash: the remainder runs as one shell command in the Session workspace and streams into a single `$ command` transcript block (ADR 0026). A `!` result is committed to Session history as a pi v3 `bashExecution` message and may enter later model context; a `!!` result is committed the same way but is excluded entirely from model conversion. User Bash is always available in the Native TUI, as is the model-requested `bash` tool under the fixed #331 tool set; the C++-only `--enable-bash` gate was deleted under ADR 0036.

The parsed command is trimmed and may span multiple lines; a bare `!` or `!!` falls through to an ordinary Agent Prompt, and `!!!foo` is excluded User Bash running `!foo`. Only direct focused-editor submissions interpret the prefix: positional initial input, one-shot print, and Skill or Prompt Template expansions beginning with `!` remain ordinary prompt text, and the prefixes are never slash commands, autocomplete items, or hotkey actions. At most one User Bash runs at a time; it may overlap an Agent run, with completion committed after the whole run settles, and the effective interrupt binding cancels an active Agent run before an active User Bash. Execution uses the same effective `shellPath`/`shellCommandPrefix` configuration, filtered environment, and `/bin/bash` → PATH `bash` → `sh` (`-c`) resolution as the model tool, has no default timeout, starts every command from the canonical Session workspace, and cancels through process-tree termination. Output is ANSI-stripped per pi's `sanitizeBinaryOutput` with carriage returns removed, is **not** secret-redacted (ADR 0028's raw pipeline), and is bounded to a 2,000-line/50 KiB tail, with truncated full output spilled to a unique owner-only temporary file.

In the one-shot print path, leading `!` text remains ordinary prompt text.

## Removed surfaces

The embeddable C++ SDK (`Sdk.hpp`), the JSONL RPC mode, and the JSON CLI mode are removed with no placeholder surface under the pi-coding-agent phase (ADR 0036); the CLI surface is exactly the pi-aligned flag set documented below. Deterministic provider behavior is exercised in the test suite — fake-provider seams live in tests only.

### Frontend selection

Frontend selection follows pi at `83114817` after argument parsing and before Agent Session creation (ADR 0036). `--mode text` is the pi-default spelling and leaves TTY-based selection unchanged; `--mode json` and `--mode rpc` are rejected with an explicit error. Otherwise `-p` / `--print` or either non-TTY stream selects one-shot print output. On supported Linux/macOS, interactive stdin/stdout selects the Native TUI, and a positional prompt becomes its initial message; other interactive platforms fail clearly. Loaded skills are explicitly invocable with `/skill:<name> [additional instructions]`; that input expands to a regular user prompt containing the skill instructions. Project resources can be trusted for one run with `--approve` / `-a` and denied with `--no-approve` / `-na`; `--no-skills` drops user and project skill discovery (explicit `--skill` paths stay). `--session <path-or-id>` opens or creates a session at the target, `--resume` opens the session picker, `--continue` resumes the most recent session, `--fork` forks from a target id, `--no-session` runs in memory, `--name` names the session, and `--session-dir` redirects automatic storage. `--theme` (repeatable) and `--no-themes` follow pi's theme semantics, and `--list-models [search]` prints pi's six-column model table.

## Skills

Skills follow pi's discovery (ADR 0036): user skills live in `~/.pi/agent/skills` (pi discovery mode, root-level `.md` files included), project skills in `<workspace>/.pi/skills` (Project Trust gated — their mere presence triggers the boot trust prompt), and `--skill` supplies explicit paths (repeatable). The `.agents/skills` convention is Supported too: user `~/.agents/skills` plus project ancestor `.agents/skills` directories up to the git root (Project Trust gated), each with its own base directory. `--no-skills` drops user and project discovery but keeps explicit paths. A skill file uses flat YAML frontmatter (`name`, `description`, optional `disable-model-invocation`) followed by markdown instructions; valid skills are listed to the model through the System Prompt's skills section and invocable as `/skill:<name> [additional instructions]` while the Skill Commands setting is enabled. The file body is read at invocation time (no cached content), with the preamble `References are relative to <baseDir>.`; malformed or duplicate skills produce bounded diagnostics.

Prompt templates follow the same source split: user `~/.pi/agent/prompts`, project `.pi/prompts` (Project Trust gated), and explicit `--prompt-template` inputs (files or directories). A missing, malformed, or unsupported explicit input aborts creation before the session is published; malformed auto-discovered templates are skipped instead and produce bounded diagnostics.

Project Context Files follow pi (`loadProjectContextFiles` at `83114817`): the global `AGENTS.md`/`CLAUDE.md` from the Agent Config Directory plus the cwd ancestor chain (AGENTS.md/AGENTS.MD/CLAUDE.md/CLAUDE.MD candidates, first regular file wins), with linked-worktree shadowing so the main repo's tracked copy of a nested worktree's own context file is not loaded twice. They are **not** Project Trust gated (pinned fact) and render into the System Prompt as `<project_context>` / `<project_instructions path="...">`; `--no-context-files` / `-nc` disables discovery. SYSTEM.md/APPEND_SYSTEM.md discovery follows pi too: project `<workspace>/.pi/SYSTEM.md` (Project Trust gated — mere presence triggers the boot trust prompt) then global `<agent config directory>/SYSTEM.md`, and the same order for APPEND_SYSTEM.md. `--system-prompt` (text-or-file, wins over discovery, empty value suppresses it) replaces the default prompt; repeatable `--append-system-prompt` entries (text-or-file, win over discovery) append to it with `"\n\n"` joining.

## Native TUI themes

The Native TUI theme surface follows pi at `83114817` (ADR 0036). Themes are pi-format JSON documents — optional `$schema`, required `name`, optional `vars` (resolved recursively), and a `colors` object over pi's 52-token set (50 required tokens plus optional `scrollbarThumb` and `thinkingMax` with their fallbacks) — validated with pi's verbatim wording; theme names containing `/` are rejected (the slash is reserved for the automatic light/dark pair). Built-in `dark` and `light` themes ship in the binary.

Discovery sources: the custom themes directory `<agent config directory>/themes`, project `.pi/themes` (Project Trust gated; auto-discovery only, skipped by `--no-themes`), and explicit `--theme` paths (repeatable; file or directory, resolved against cwd — a missing path is a warning, not an abort). The default selection uses pi's env-only detection: the last 0–255 integer in `COLORFGBG` → ANSI luminance ≥ 0.5 → light, else dark. Applying a theme that fails to load falls back to `dark` with `Failed to load theme "<name>": <error>` + `Fell back to dark theme.`. The `/settings` ThemeSubmenu lists the available themes (builtins, custom directory, registered) with in-memory preview on selection, a global-scope settings commit on confirm, and no revert on cancel (pi quirk). `/reload` re-runs theme discovery and re-applies the active theme. There is no theme file watcher, no OSC 11/DSR terminal query, and no automatic light/dark pair.

## Native TUI keybindings

One startup pass reads `keybindings.json` only from this product's Agent Config Directory. The baseline-compatible format accepts one key string, multiple alternatives, or an empty array per namespaced action. Valid user entries replace only that action's defaults; context-local default reuse remains intact, while user/user conflicts are diagnosed and resolved by contextual candidate order. Invalid keys, unknown IDs, and known but unassembled application IDs are skipped with bounded, redacted diagnostics and never create inert bindings.

The exact immutable effective registry drives component dispatch, hotkey help, and rendered key hints. The app layer adopts pi's full 42-action `app.*` catalog (`core/keybindings.ts` at `83114817`); the main editor assembles pi's default bound set — `app.interrupt`, `app.clear`, `app.exit`, `app.suspend`, `app.thinking.cycle`, `app.model.cycleForward`/`cycleBackward`/`select`, `app.tools.expand`, `app.thinking.toggle`, `app.editor.external`, `app.message.copy`/`followUp`/`dequeue`, `app.clipboard.pasteImage` — while `app.session.*` stays recognized-but-unbound in the main editor (pi ships `defaultKeys: []`) and the session and tree selectors bind their scoped actions; `/hotkeys` and the header hints render the assembled subset only. Idle Enter or Alt+Enter starts an ordinary prompt; Enter during active work admits steering input, while Alt+Enter admits follow-up input. `shift+enter` and `ctrl+j` remain multiline editor actions. Accepted input leaves the editor and appears in the pending queue display; rejected input is restored unchanged with a bounded, redacted diagnostic. Dequeue restores steering messages, then follow-up messages, then existing editor text, separated by blank lines. Interrupt restores pending text before requesting abort. Linux/macOS job-control defaults and native Windows unavailability are explicit when an application action is concretely registered. See [Native TUI keybindings](docs/keybindings.md) for the grammar, implemented IDs, precedence, diagnostics, platform behavior, and pi baseline sources. There is no explicit TUI mode and no hot reload.

## Project trust and resource controls

Project trust controls whether project-authored `.pi/` resources are loaded at startup. It is an input-loading guard, not a sandbox: it does not restrict built-in tools, model output, prompt injection from files you choose to read, shell commands, or transcript content already present in a resumed session.

Protected project markers include `.pi/settings.json`, `.pi/skills`, `.pi/prompts`, `.pi/themes`, `.pi/SYSTEM.md`, and `.pi/APPEND_SYSTEM.md`, plus project ancestor `.agents/skills` directories; the extensions/packages markers and the `.cpp-harness/` names were removed with the extension host and package manager under ADR 0036. A bare `.pi` directory and `.pi/sessions` do not require trust. When a trust-requiring resource exists and no override is set, the interactive frontend shows the boot trust prompt (the generic selector as an overlay on the main TUI); in non-interactive startup the default `ask` policy acts as untrusted unless a saved trust decision or same-run override exists. Trust decisions are read from user-controlled `~/.pi/agent/trust.json` with nearest-parent inheritance.

User settings follow pi's two-scope `settings.json` contract (ADR 0031): a global file at `~/.pi/agent/settings.json` deep-merged with a project file at `<workspace>/.pi/settings.json` that loads only while the project is trusted, with the project scope winning. The field subset is `defaultProvider`, `defaultModel`, `defaultThinkingLevel` (`off`/`minimal`/`low`/`medium`/`high`/`xhigh`/`max`), `enabledModels` (model patterns), `sessionDir`, `defaultProjectTrust` (`ask`/`always`/`never`; global-only), `shellPath`, `shellCommandPrefix`, `theme`, `hideThinkingBlock`, `outputPad`, and `enableSkillCommands`. The harness-private `provider`/`model`/`base_url`/`api_key_env`/`auth` fields and the `project_resources` skill/theme policy fields are removed, and settings never carry secrets. Optional compatible `shellPath` and `shellCommandPrefix` strings configure local Shell launches from the startup settings snapshot; a leading `~` in `shellPath` expands to the user's home directory. The optional `sessionDir` string is a CLI session-storage preference below `--session-dir` and `PI_CODING_AGENT_SESSION_DIR`. `--approve` / `-a` and `--no-approve` / `-na` are one-run trust overrides; they do not persist decisions. `--no-skills` drops user and project skill discovery (explicit `--skill` paths stay), and `--no-context-files` / `-nc` disables Project Context File discovery.

## Tools

The built-in tools are pi's default set with pi names (`read`/`bash`/`edit`/`write`; `grep`/`find`/`ls` are absent with no placeholder):

- `read`: read a text file inside the workspace, with optional line offset/limit. Appends a continuation hint when output is truncated.
- `write`: create or overwrite a file inside the workspace; creates parent directories implicitly.
- `edit`: perform one or more exact replacements via an `edits[]` array of `{oldText, newText}` pairs using pi's edit-diff semantics (edit-diff.ts): every edit is matched against the original file (not incrementally), exact match first then fuzzy-normalized matching (NFKC, per-line trailing whitespace stripped, smart quotes/dashes/spaces normalized), the dominant line ending (CRLF vs LF) and a UTF-8 BOM are preserved, zero/multiple matches per edit are rejected with pi's messages, overlapping edits are rejected, and an unchanged result is an error. The result carries `Successfully replaced N block(s) in <path>.` plus details with the display diff, a unified patch, and the first changed new-file line number.
- `bash`: run a shell command inside the workspace. The tool is always available under the fixed #331 tool set (`--enable-bash` was deleted under ADR 0036). Accepts a timeout in seconds and strips ANSI escape sequences from output. On supported Unix platforms, local execution resolves an expanded configured `shellPath`, otherwise `/bin/bash`, PATH `bash`, then PATH `sh`, and invokes it with `-c` rather than as a login shell. A stale configured path fails only the attempted execution; it does not prevent Session startup.

The registry owns tool capabilities directly. Tool calls in one assistant message execute in parallel by default (pi's `toolExecution` default `"parallel"`, expressed as `BoundedParallelToolExecution` with no explicit cap); a batch containing a call to a tool whose adapter declares `Exclusive` (pi `executionMode: "sequential"`) runs through the sequential path with full per-call lifecycle in source order, and an explicit `SequentialToolExecution` or a cap of one does the same. Every call starts its Tool Execution lifecycle before the executor parses and clones the raw arguments, applies the recorded pi-baseline recursive coercion profile, and validates the tool's JSON Schema before policy hooks or capability invocation. The executable profile covers primitive and union types, nested objects and arrays (including tuple items and dynamic properties), value and collection constraints, composition, and baseline-recognized formats. Unknown formats and members remain annotations unless a required vocabulary makes support mandatory. Malformed JSON, unknown tools, invalid or unsupported dialects and vocabularies, unresolved references, unsupported executable constructs, and schema-invalid values become bounded error Tool Call Outcomes for only their calls; annotations and extension members do not disable recognized assertions. Parser excerpts in malformed-argument diagnostics are secret-redacted before UTF-8-safe truncation, so the same safe diagnostic flows through tool lifecycle events, model context, and Session persistence.

Tool adapters declare their concurrency through `concurrency()`: the default is `Exclusive` (pi `executionMode: "sequential"`); parallel-safe adapters explicitly return `ParallelSafe`. The built-in tools remain `Exclusive` because their shared execution environment has no public concurrent-use contract. A tool may override `execute_with_updates()` to publish cumulative partial tool execution results through the move-only update sink; those updates retain the Tool Call ID through Agent lifecycle and Native TUI presentation, late updates after execution settlement are ignored, and ordinary tools continue to implement `execute()`. The active prompt's `std::stop_token` is mandatory at the tool and filesystem capability seams. Sequential execution observes cancellation before starting each call and after the current hook/tool outcome is finalized. Bounded-parallel workers observe it before claiming and before invoking queued capabilities; already-running calls quiesce, while claimed-but-unstarted calls receive balanced `Operation aborted` outcomes without capability invocation.

File tools deliberately share one execution environment capability, which owns workspace containment, path validation, atomic writes, process execution, timeout handling, and output limiting. Awaitable filesystem operations observe cancellation immediately before and after their synchronous local filesystem boundary; a completed mutation is not rolled back when cancellation wins the post-operation check. Best-effort `cleanup()` is deliberately the sole no-token operation. Shell execution carries the same token through `ExecOptions` and `ProcessRequest`; on supported platforms cancellation terminates the active process group and reaps the shell before returning `Aborted`. File tools reject workspace escapes, symlink escapes, directory/file mismatches, and missing parents unless creation is explicitly requested. `bash` receives a sanitized environment that omits API-key, token, secret, password, and OpenAI-looking variables.

## Sessions and safety

Sessions are JSONL:

1. a pi-style v3 `session` header with session/workspace/provider/model metadata (legacy v2 headers remain readable),
2. append-only typed `message` entries containing redacted user, assistant, and tool-result messages,
3. write support for v3 tree metadata entries (`model_change`, `thinking_level_change`, `active_tools_change`, `custom`, `custom_message`, `label`, `compaction`, `branch_summary`, `session_info`) and extended runtime messages (`BashExecutionMessage`, `CustomMessage`, `BranchSummaryMessage`, `CompactionSummaryMessage`),
4. safely ignored unknown future entry types.

Each completed user, assistant, or tool-result message enters the stateful Agent's live history before weak subscribers run and is appended after observer delivery. Subscriber failures or exceptions become bounded Agent diagnostics and deactivate the faulty observer; they do not stop later observers, veto Agent progress, or prevent persistence. A persistence failure remains a strong commitment failure: it stops the current run without rolling back live history or closing the session. Later in-process prompts use retained live state, while reopening reconstructs only entries that reached durable storage. Current resume uses the v3 session tree to rebuild the active path, including persisted leaf selection and compaction summaries, through the session resume module. Exact unredacted replay is intentionally out of scope. The session-family CLI follows pi (ADR 0036): `--session <path-or-id>` opens or creates at the target, `--resume` opens the session picker, `--continue` resumes the most recent session, `--fork` forks from a target id, `--session-id` validates/conflicts against the target, `--name` names the session, `--no-session` runs in memory without persisting, and `--list-models [search]` prints pi's six-column model table (`provider model context max-out thinking images`) with fuzzy search. Session files are still sensitive: they can contain source text, command output, workspace paths, and provider/model metadata.

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
./build/cpp_harness_tests "[cli]"
```

These cover:

- public headers compile from the include contract surface without `src`, legacy sync contracts, Boost.JSON, or raw Glaze generic values in domain contracts;
- stateful Agent prompts retain live history, expose passive snapshots, reject overlapping runs, and notify weak move-only observers in lifecycle order;
- fake model/tool loops route through provider-neutral value contracts and owned tool capabilities (the scripted fake Provider lives in the test suite);
- move-only event sinks can capture unique state and propagate errors;
- provider-specific OpenAI/SSE/Glaze wire mapping stays isolated from the agent loop;
- JSONL resume reconstructs typed message ordering for the next request;
- AgentSession tests protect live-state-before-subscriber-before-persistence ordering, passive fresh/in-memory/active/resumed snapshots, and failure recovery;
- CLI slices exercise the pi-aligned frontend/session surface without compatibility-only flags.

## Deferred

Not included: Native Windows TUI support; extensions; packages; live skill reload; MCP integration; permission prompts; native Windows shell process-tree termination semantics; subagents; HTML session export and `/export` `/import` `/share`; branch summarization generation; C++26 reflection-generated schema; `std::execution` senders/receivers; ABI-stable binary distribution; or OS-level sandboxing. OAuth login (Codex PKCE callback server, Kimi RFC 8628 device flow) is a Supported Capability of the pi-ai module (see [fixtures/pi-ai/README.md](fixtures/pi-ai/README.md)); its interactive presentation (login dialog, OAuth selector, API-key branch) is Supported in the Native TUI per ADR 0036.

The embeddable C++ SDK, the JSONL RPC mode, and the JSON CLI mode are removed with no placeholder surface under ADR 0036.

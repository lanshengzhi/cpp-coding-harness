# C++ Coding Harness

A small experimental C++23 coding-agent harness built around anti-fragile architecture rules:

1. **Data is passive value state**: public contracts are aggregate-friendly structs, `std::variant` alternatives, `std::expected` failures, and a project `JsonValue` for unstructured JSON facts.
2. **Capabilities cross physical seams**: chat clients, stream transports, execution environments, session stores, and tools are replaceable interfaces or dependency-heavy concrete implementations hidden behind headers.
3. **Events are weak connections**: agent/provider event sinks use move-only callback semantics so subscribers can own unique state without forcing `std::shared_ptr` or copyability.
4. **Generic machinery stays local**: Glaze DTOs, schema conversion, visitors, parsing helpers, and future reflection-friendly machinery live in serialization/implementation layers rather than domain-facing APIs.

The harness loop mirrors the core pi-style flow:

1. accept a prompt from CLI or REPL,
2. send ordered messages plus JSON Schema tool definitions to an OpenAI-compatible chat API,
3. execute local tools requested through `tool_calls`,
4. append tool-result messages with matching call IDs,
5. repeat until the assistant stops or the max-turn limit is reached,
6. persist the redacted typed transcript as JSONL.

This is a learning and experimentation harness, not a production sandbox or compatibility-preserving SDK.

## Build

The project is CMake-based and requires a C++23-capable compiler. CMake 3.25 or newer is expected.

- Glaze is used only at typed JSON serialization/deserialization boundaries.
- Boost.Beast/Asio + OpenSSL provide the HTTPS transport implementation.
- Boost.Process is used behind the process-execution capability boundary.
- CLI11 and Catch2 are declared in `vcpkg.json`; this repository also carries a tiny Catch-compatible fallback test header so the default suite can run in minimal environments.

### Bootstrap with vcpkg (recommended)

All dependencies are declared in `vcpkg.json`. The bootstrap scripts create a local `.deps/vcpkg` checkout when `VCPKG_ROOT` is not already set, bootstrap vcpkg, and configure CMake in manifest mode so dependencies such as CLI11, Glaze, Boost, and OpenSSL are installed automatically.

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

If you prefer system-installed dependencies, install Boost, OpenSSL, Glaze, and CLI11 yourself, then use the system preset:

```bash
cmake --preset system
cmake --build --preset system
ctest --preset system
```

Run the binary with the deterministic fake provider:

```bash
./build/cpp_harness --fake --session /tmp/cpp-session.jsonl "hello"
./build/cpp_harness --fake --workspace . --session /tmp/cpp-read.jsonl "read README.md"
./build/cpp_harness --fake --mode json --session /tmp/cpp-json.jsonl "hello" | jq -c 'select(.type == "message_update")'
printf '{"type":"get_state"}\n{"type":"shutdown"}\n' | ./build/cpp_harness --fake --mode rpc --session /tmp/cpp-rpc.jsonl
./build/cpp_harness --fake --repl --session /tmp/cpp-repl.jsonl
```

The coroutine/Glaze/event stack is the only active stack. Legacy compatibility flags such as `--async` are intentionally absent.

Real-provider mode is OpenAI Chat Completions-compatible:

```bash
export OPENAI_API_KEY=...
./build/cpp_harness --model gpt-4.1-mini --session /tmp/cpp-real.jsonl "summarize README.md"
```

Use `--base-url` for compatible gateways that preserve the `/v1/chat/completions` contract.

OAuth, subscription-provider, and dynamic API-key resolution flows are intentionally deferred. The `--api-key-env` mechanism reads a static environment variable at provider construction time; there is no OAuth handshake, token refresh, or per-call key callback yet.

### Auth file

API keys can also be stored in `~/.cpp-harness/agent/auth.json` and selected by name with the `--auth` flag (or the `auth` field in `~/.cpp-harness/config.json`). This avoids exporting keys into the shell environment.

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
  --session /tmp/cpp-kimi.jsonl \
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
  --session /tmp/cpp-kimi.jsonl \
  "summarize README.md"
```

Pass the base URL (`https://api.kimi.com/coding/v1`), not the full `/chat/completions` endpoint; the harness appends `/chat/completions` for `/v1`-style base URLs.

Kimi's `ANTHROPIC_BASE_URL` / `ANTHROPIC_API_KEY` examples are for Anthropic-shaped Claude Code clients. This harness does not read those variables or implement an Anthropic provider.

Live Kimi usage sends prompts, file contents read by tools, and tool outputs to the configured provider. JSONL session redaction is a persistence boundary, not a guarantee that terminal output, CI logs, provider diagnostics, or provider-bound tool results are redacted. Do not paste raw credentials into prompts, files, or tool-visible content.

`--resume` reconstructs the redacted active session path, including persisted leaf and compaction context, and restores workspace metadata. When you omit `--model`, `--base-url`, `--api-key-env`, or `--auth`, the harness falls back to values stored in the resumed session, then to `~/.cpp-harness/config.json`, then to built-in defaults. Explicit CLI flags always win. For Kimi sessions, repeating the Kimi base URL, model, and key source on resume is still recommended so runtime context stays explicit.

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
- `cch_ai` (`include/cch/ai`, `src/ai`): passive message/content/tool/context contracts, provider-neutral stream events, provider registry, OpenAICompletionsCompat flags, OpenAI-compatible provider, scripted fake provider; SSE and Glaze provider mapping live under `src/ai/`.
- `cch_agent` (`include/cch/agent`, `src/agent`): coroutine agent loop, observable state values, lifecycle event values, move-only event sinks, async tool registry, expected-style tool execution contracts, optional pre/post tool-call hooks (`beforeToolCall`/`afterToolCall`), context transform / LLM conversion hooks, steering/follow-up queues, prepare-next-turn updates, and a safe-default sequential/bounded-parallel tool execution policy.
- `cch_harness` (`include/cch/harness`, `src/harness`): pi-shaped filesystem and shell execution capability contracts (`FileSystem`/`Shell`), local implementation with workspace containment, symlink safety, atomic writes, split-stream process execution, secret environment filtering, and JSONL session persistence.
- `cch_tools` (`include/cch/tools`, `src/tools`): built-in read/write/edit/bash tool factories bridging agent tool contracts to harness capabilities.
- `cch_coding_agent_runtime` (`src/cli/` for CLI11 parsing/preflight/`CliConfig`/`CliRuntimeConfig`, `src/coding_agent/runtime/AsyncCliRuntime.*`, `src/coding_agent/runtime/`, `include/cch/coding_agent/`, `src/coding_agent/`): CLI argument parsing and preflight (including `config.json` precedence), runtime orchestration, session lifecycle, provider/tool service assembly, semantic event printing, JSON/RPC output modes, private skill/template prompt interpretation (`prompt/PromptProcessor`), live-state-first event handling with incremental message persistence, project trust/resource controls (`ProjectTrust`, `ProjectResources`, `ProjectResourceLoader`), project-local skill discovery/loading and prompt formatting, `/skill:name` expansion from cached content, and prompt-template file loading with `--prompt-template`/`--no-prompt-templates` CLI flags.

The build publishes `include` as the public surface and keeps `src` private. Legacy synchronous tools, Boost.JSON contracts, `util::Result`, and duplicate `src` contract headers have been removed.

### pi parity roadmap

Long-term work tracks pi module and contract parity in `docs/plans/2026-06-16-001-refactor-pi-cpp-parity-todo.md`. The pre-implementation cleanup in `docs/plans/2026-06-16-002-refactor-pre-implementation-cleanup-plan.md` established the structural prerequisites for larger parity slices: package-style CMake targets, CLI11 parsing in `src/cli/`, provider registry wiring with a registered fake provider, true async shell execution, expanded agent event/state seams, runtime service split, and v3 session tree with write support, branch navigation, and compaction-aware context reconstruction.

## CLI states

The default text CLI prints stable semantic event lines:

- `[model-request]`
- `[assistant] <text delta>`
- `[tool-call] <name>#<id>`
- `[tool-success] <id>`
- `[tool-error] <id>`
- `[completed]`

Prompt completion failures are reported on stderr as `loop failed: <message>` and return a non-zero exit status; they do not introduce a second text-event status model.

`--mode json` emits one compact JSON object per stdout line. The first record is the pi v3 session header; every later record is a directly serialized supported AgentSession event such as `agent_start`, `turn_start`, `message_start`, `message_update`, `tool_execution_start`, `tool_execution_end`, `turn_end`, and `agent_end`. Event payloads retain their message/tool structure after secret redaction and bounded-output transformation. JSON mode has no C++ schema/version envelope, sequence counter, content-status substitution, or `runtime_terminal` record. Prompt failures are reported on stderr with a non-zero exit. Frontend-only slash commands produce no post-header JSON record because they do not enter AgentSession.

`--mode rpc` is a narrow JSONL stdin/stdout command loop. It supports `prompt`, `get_state`, `get_last_assistant_text`, and `shutdown`; unsupported pi RPC commands return structured `success:false` responses. RPC mode emits no startup session header and interleaves command responses with direct AgentSession events on stdout. Prompt success is acknowledged after session preflight accepts the prompt and before its first event; preflight rejection returns `success:false`. Later execution success or failure never emits a second response or a `runtime_terminal` record. Startup/pre-session failures remain on stderr. The protocol is sequential only: no `abort`, `steer`, `follow_up`, session switching, fork/clone, compaction, or extension UI yet.

### Slash commands

The effective built-in command names are:

| Command | Behavior |
| --- | --- |
| `/help [command]` | List all effective registry names, or show detailed help for one name. Both `/help name` and `/help /name` are accepted. |
| `/commands` | Alias for `/help`. |
| `/session` | Show the current session id, workspace, provider, model, and message count. |
| `/new` | **Instructional placeholder:** explain how to restart without `--resume`; it does not replace the current session. |
| `/resume <session-id>` | **Instructional placeholder:** explain how to restart with `--resume`; it does not switch the current session. |
| `/clear` | Clear the terminal only in the text frontend. Arguments produce `Usage: /clear`. |
| `/quit` | Request a clean frontend shutdown. |
| `/exit` | Alias for `/quit`. |

Aliases are resolved by the command registry and appear in `/help`. Registry help does not list prompt templates or `/skill:<name>` invocations as built-in commands. Prompt interpretation in `AgentSession` activates only for a slash at column zero and applies cached skill → prompt-template precedence; CLI built-in commands are resolved by the CLI adapter before ordinary input reaches the session. Bare `/` and unmatched slash input pass through to the model unchanged.

`!command` and `!!command` are not prompt-processing syntax. The text REPL temporarily intercepts them with a not-implemented message; one-shot, JSON, RPC, and SDK paths currently send them as ordinary prompts until the separate UserBash persistence slice lands.

Command presentation depends on the frontend:

| Input | Text REPL / one-shot | JSON mode | RPC `prompt` |
| --- | --- | --- | --- |
| Help, session information, or restart instructions | Display the command message without invoking the model. | Emit only the session header; frontend command outcomes are not AgentSession events. | Sent as ordinary prompt text; RPC built-ins are JSON commands, not text-frontend slash commands. |
| Exact `/clear` | Write the terminal clear sequence from the text frontend, flush, and succeed without prompting the session. | Emit only the session header and no ANSI bytes. | Sent as ordinary prompt text; no terminal-control bytes are emitted by the adapter. |
| `/quit` or `/exit` | Display `Shutting down.` and exit successfully. | Emit only the session header, then exit successfully. | Sent as ordinary prompt text; use the RPC `shutdown` command to stop the loop. |

## Embeddable C++ SDK

The project exposes an experimental same-process C++23 SDK surface for host applications that want to embed the agent loop without shelling out to `cpp_harness` or depending on CLI/RPC globals. The SDK is source-level only (not ABI-stable) and lives under `include/cch/coding_agent/Sdk.hpp`.

```cpp
#include <cch/coding_agent/Sdk.hpp>

using namespace cch;

// Create a session with provider config (resolves API key from env)
coding_agent::CreateAgentSessionOptions opts;
opts.session_path = "/tmp/my-session.jsonl";
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

auto& session = result->session;

// Subscribe to lifecycle events
int events = 0;
auto sub = session->subscribe(
    [&](const agent::AgentLifecycleEvent&) -> util::ExpectedVoid {
        ++events;
        return {};
    });

// Run a prompt. Progress arrives through the persistent subscription.
if (auto prompted = session->prompt("hello"); !prompted) {
    // handle prompted.error()
}
auto last_text = session->last_assistant_text();
auto message_count = session->message_count();

// Close (idempotent — repeated close is safe)
session->close();
```

**Supported SDK v1 behavior:**

- Explicit create/resume path: exactly one of `session_path` (create new) or `resume_path` (resume existing) must be set.
- Blocking `prompt()` — serial, single-prompt-at-a-time, returning only success or an explicit `util::Error`.
- One persistent event-subscription path via move-only `agent::AgentEventSink`; prompt progress is not returned or delivered through per-prompt sinks.
- Host-provided chat clients and execution environments. Chat clients and custom tools passed by `unique_ptr` transfer ownership to the session; `shared_ptr` execution environments remain host-owned and are not cleaned up by session close.
- SDK convenience provider construction from `SdkProviderConfig` when no host client is supplied.
- Built-in tool selection with safe defaults: `read`, `write`, and `edit_file` by default; `bash` requires explicit opt-in.
- Custom tool registration via existing `agent::AsyncAgentTool` contracts; duplicate tool names fail creation.
- Programmatic skills and prompt templates; host resources take precedence over project-discovered duplicates.
- Per-prompt `PromptOptions::expand_prompt_templates` (default `true`); `false` bypasses skill and prompt-template expansion and sends raw text to the agent loop. Resulting message count and last assistant text are queried through `AgentSession` state accessors.
- Optional project resource discovery (`.cpp-harness/skills/`, `.cpp-harness/prompts/`) behind explicit trust/resource controls.
- Optional absolute external `trust_store_path` for SDK project-resource trust decisions; workspace-local trust-store paths are rejected.
- Diagnostics returned as `CreateAgentSessionResult::diagnostics` values — no stdout/stderr output from the SDK path.
- Resolved session metadata on `CreateAgentSessionResult::metadata`, matching the created/resumed JSONL session header.

**Not supported in SDK v1:**

- ABI-stable binary distribution, plugin ABI, or package-manager integration.
- Full pi `AgentSessionRuntime` replacement APIs (`newSession`, `switchSession`, `fork`, `clone`, import/export).
- Public branch/tree navigation or SDK append support for non-linear session topologies (SDK v1 returns `unsupported_session_topology` for branched/compacted sessions).
- In-memory sessions, concurrent prompts, cancellation, `abort`, `steer`, `followUp`, or queueing.
- Dynamic TypeScript/JavaScript extensions, extension UI, hot reload, MCP, or package installation.
- TUI run modes, themes, keybindings, widgets, OAuth/subscription providers, or model catalogs.

One-shot text mode runs one prompt. When no positional prompt is given in text mode, the CLI defaults to `--repl` and keeps history in memory for multiple prompts. Loaded skills are explicitly invocable with `/skill:<name> [additional instructions]`; that input expands to a regular user prompt containing the skill instructions. Project-local resources can be trusted for one run with `--approve` / `-a`, denied for one run with `--no-approve`, and project skills can be suppressed with `--no-skills`. `--mode json` and `--mode rpc` cannot be combined with `--repl`; RPC mode reads prompts from stdin commands rather than positional CLI arguments. `--resume <session.jsonl>` starts from the active tree path before persisting new messages through the current session store. `--session <path>` always creates a new file; use `--resume` to append.

## Skills

At startup, the CLI scans project-local `.cpp-harness/skills` for nested `SKILL.md` files only when the project resource load plan allows project skills. A skill file uses flat YAML frontmatter (`name`, `description`, optional `disable-model-invocation`) followed by markdown instructions. Valid skills are loaded into the session, diagnostics for malformed or duplicate skills print to stderr, and visible skills are injected into model context through the pi-shaped `<available_skills>` block.

Skills with `disable-model-invocation: true` are hidden from the model-visible list but can still be invoked explicitly with `/skill:<name> [additional instructions]` when the skill was loaded. Invocation uses the skill body cached at startup; edit/reload behavior during a running session, global `~/.cpp-harness/skills`, and config-driven skill directories are deferred.

## Project trust and resource controls

Project trust controls whether project-authored `.cpp-harness` resources are loaded at startup. It is an input-loading guard, not a sandbox: it does not restrict built-in tools, model output, prompt injection from files you choose to read, shell commands enabled with `--enable-bash`, or transcript content already present in a resumed session.

Protected project markers include `.cpp-harness/settings.json`, `.cpp-harness/skills`, `.cpp-harness/prompts`, `.cpp-harness/extensions`, `.cpp-harness/packages`, `.cpp-harness/SYSTEM.md`, and `.cpp-harness/APPEND_SYSTEM.md`. A bare `.cpp-harness` directory and `.cpp-harness/sessions` do not require trust. In non-interactive startup, the default `ask` policy acts as untrusted unless a saved trust decision or same-run override exists. Trust decisions are read from user-controlled `~/.cpp-harness/trust.json` with nearest-parent inheritance.

User config in `~/.cpp-harness/config.json` can set provider defaults (`provider`, `model`, `base_url`, `api_key_env`, `auth`), `default_project_trust` to `ask`, `always`, or `never`, and `project_resources.skills` to `auto`, `on`, or `off`. `off` always skips project skills. `--approve` / `-a` and `--no-approve` are one-run trust overrides; they do not persist decisions. `--no-skills` disables project-local skills for the run even when the project is trusted.

## Tools

The built-in tools are:

- `read_file`: read a text file inside the workspace, with optional line offset/limit. Appends a continuation hint when output is truncated.
- `write_file`: create or overwrite a file inside the workspace; creates parent directories implicitly.
- `edit_file`: perform one or more exact replacements via an `edits[]` array of `{oldText, newText}` pairs; zero or multiple matches per edit are rejected. Multiple disjoint edits in one call are applied together.
- `bash`: run a shell command inside the workspace only when `--enable-bash` is passed. Accepts a timeout in seconds and strips ANSI escape sequences from output.

The registry owns tool capabilities directly. Tools default to `Exclusive`; bounded parallel execution requires both an explicit run policy and a tool adapter that declares `ParallelSafe`. The built-in tools remain `Exclusive` because their shared execution environment has no public concurrent-use contract. File tools deliberately share one execution environment capability, which owns workspace containment, path validation, atomic writes, process execution, timeout handling, and output limiting. File tools reject workspace escapes, symlink escapes, directory/file mismatches, and missing parents unless creation is explicitly requested. `bash` receives a sanitized environment that omits API-key, token, secret, password, and OpenAI-looking variables.

## Sessions and safety

Sessions are JSONL:

1. a pi-style v3 `session` header with session/workspace/provider/model metadata (legacy v2 headers remain readable),
2. append-only typed `message` entries containing redacted user, assistant, and tool-result messages,
3. write support for v3 tree metadata entries (`model_change`, `thinking_level_change`, `active_tools_change`, `custom`, `custom_message`, `label`, `compaction`, `branch_summary`, `session_info`) and extended runtime messages (`BashExecutionMessage`, `CustomMessage`, `BranchSummaryMessage`, `CompactionSummaryMessage`),
4. safely ignored unknown future entry types.

Each completed user, assistant, or tool-result message enters live history before subscribers run and is appended after successful subscriber delivery. Subscriber or persistence failure does not roll back live history or close the session; later in-process prompts use retained live state, while reopening reconstructs only entries that reached durable storage. Current resume uses the v3 session tree to rebuild the active path, including persisted leaf selection and compaction summaries, through the session resume module. Exact unredacted replay is intentionally out of scope. Session files are still sensitive: they can contain source text, command output, workspace paths, and provider/model metadata.

The workspace guard is not a sandbox. Prompts, file contents, and command outputs can be sent to the configured provider. Run this harness inside a VM/container if you need a real containment boundary.

## Executable specs

Useful default validation slices:

```bash
./build/cpp_harness_tests "[architecture]"
./build/cpp_harness_tests "[ai][u2]"
./build/cpp_harness_tests "[ai][provider]"
./build/cpp_harness_tests "[agent][async]"
./build/cpp_harness_tests "[tools][async]"
./build/cpp_harness_tests "[harness][session]"
./build/cpp_harness_tests "[sdk][live-state]"
./build/cpp_harness_tests "[sdk][incremental-persistence]"
./build/cpp_harness_tests "[coding-agent][json-events]"
./build/cpp_harness_tests "[coding-agent][runtime][rpc]"
./build/cpp_harness_tests "[cli]"
```

These cover:

- public headers compile from the include contract surface without `src`, legacy sync contracts, Boost.JSON, or raw Glaze generic values in domain contracts;
- fake model/tool loops route through provider-neutral value contracts and owned tool capabilities;
- move-only event sinks can capture unique state and propagate errors;
- provider-specific OpenAI/SSE/Glaze wire mapping stays isolated from the agent loop;
- JSONL resume reconstructs typed message ordering for the next request;
- AgentSession tests protect live-state-before-subscriber-before-persistence ordering and failure recovery;
- JSON/RPC transcript tests protect direct events, prompt preflight acknowledgement, response correlation, and terminal-record absence;
- CLI fake-provider smoke tests demonstrate the current event/subscriber path without compatibility-only flags.

## Deferred

Not included yet: rich TUI, extensions, packages, global/config-driven skill directories, live skill reload, OAuth, full pi RPC command parity, MCP integration, permission prompts, native Windows shell process-tree termination semantics, tool execution streaming updates, subagents, C++26 reflection-generated schema, `std::execution` senders/receivers, ABI-stable binary distribution, or OS-level sandboxing.

An experimental same-process embeddable C++ SDK surface is available (see Embeddable C++ SDK section above). Full pi SDK parity (session replacement runtime, concurrent prompts, compaction, in-memory sessions, ABI stability) is deferred.

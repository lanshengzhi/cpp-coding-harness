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

The project is CMake-based and requires a C++23-capable compiler. CMake 3.20 or newer is expected.

- Glaze is used only at typed JSON serialization/deserialization boundaries.
- Boost.Beast/Asio + OpenSSL provide the HTTPS transport implementation.
- Boost.Process is used behind the process-execution capability boundary.
- CLI11 and Catch2 are declared in `vcpkg.json`; this repository also carries a tiny Catch-compatible fallback test header so the default suite can run in minimal environments.

### Using vcpkg (recommended)

All dependencies are declared in `vcpkg.json` and resolved automatically via vcpkg manifest mode. No system-installed packages are required.

Prerequisites: [vcpkg](https://github.com/microsoft/vcpkg) installed and `VCPKG_ROOT` environment variable set.

```bash
# one-time vcpkg setup (if you don't have it yet)
git clone https://github.com/microsoft/vcpkg.git ~/vcpkg
~/vcpkg/bootstrap-vcpkg.sh
export VCPKG_ROOT=~/vcpkg

# build
cmake --preset vcpkg
cmake --build build -j4
ctest --preset vcpkg
```

### Using system packages

If you prefer system-installed Boost, OpenSSL, and Glaze:

```bash
cmake --preset system
cmake --build build -j4
ctest --preset system
```

Run the binary with the deterministic fake provider:

```bash
./build/cpp_harness --fake --session /tmp/cpp-session.jsonl "hello"
./build/cpp_harness --fake --workspace . --session /tmp/cpp-read.jsonl "read README.md"
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

### Kimi Code via the OpenAI-compatible path

Kimi Code can be used through the existing OpenAI-compatible provider path. Keep the Kimi base URL, model, and API-key environment variable together because the bearer token from `--api-key-env` is sent to whichever `--base-url` you configure.

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

`--resume` loads the redacted transcript and workspace metadata, but it does not restore `--base-url`, `--model`, or `--api-key-env`. Repeat all three Kimi flags when resuming a Kimi session.

Troubleshooting:

| Symptom | Check |
| --- | --- |
| `missing API key` | Export `KIMI_API_KEY` and pass `--api-key-env KIMI_API_KEY`. |
| Authentication or authorization failure | Confirm the key is valid for Kimi Code and that the base URL is `https://api.kimi.com/coding/v1`. |
| Invalid model | Use `--model kimi-for-coding`. |
| Rate limit or quota error | Retry later or check Kimi Code subscription/entitlement and quota. |
| Request unexpectedly goes to OpenAI | Ensure the Kimi `--base-url`, `--model`, and `--api-key-env` are all present. |
| 403 Forbidden | Your key can list models but is not entitled for Kimi Code chat completions; confirm Kimi Code subscription/agent access. |
| Provider or transport error | Re-run with harmless prompts and inspect diagnostics without printing secrets. |

Optional live smoke validation is manual and never part of default `ctest`:

```bash
CCH_LIVE_KIMI=1 KIMI_API_KEY=... scripts/kimi_live_smoke.sh
```

The smoke script requires explicit opt-in, uses a throwaway workspace/session, does not enable bash, and consumes real network/quota.

## Architecture boundaries

The code is split into value contracts, capability seams, implementation adapters, and package-style CMake targets:

- `cch_util` (`include/cch/util`, `src/util`): project error/expected contracts, move-only callback vocabulary, passive `JsonValue`, the Glaze-backed JSON adapter, and async process execution.
- `cch_ai` (`include/cch/ai`, `src/ai`): passive message/content/tool/context contracts, provider-neutral stream events, provider registry, OpenAICompletionsCompat flags, OpenAI-compatible provider, scripted fake provider, SSE, and Glaze provider mapping.
- `cch_agent` (`include/cch/agent`, `src/agent`): coroutine agent loop, observable state values, lifecycle event values, move-only event sinks, async tool registry, expected-style tool execution contracts, optional pre/post tool-call hooks (`beforeToolCall`/`afterToolCall`), context transform / LLM conversion hooks, steering/follow-up queues, prepare-next-turn updates, and sequential/parallel tool execution modes.
- `cch_harness` (`include/cch/harness`, `src/harness`): pi-shaped filesystem and shell execution capability contracts (`FileSystem`/`Shell`), local implementation with workspace containment, symlink safety, atomic writes, split-stream process execution, secret environment filtering, and JSONL session persistence.
- `cch_tools` (`include/cch/tools`, `src/tools`): built-in read/write/edit/bash tool factories bridging agent tool contracts to harness capabilities.
- `cch_coding_agent_runtime` (`src/AsyncCliRuntime.*`, `src/coding_agent/runtime/`): CLI runtime orchestration, session lifecycle, provider/tool service assembly, and semantic event printing.

The build publishes `include` as the public surface and keeps `src` private. Legacy synchronous tools, Boost.JSON contracts, `util::Result`, and duplicate `src` contract headers have been removed.

### pi parity roadmap

Long-term work tracks pi module and contract parity in `docs/plans/2026-06-16-001-refactor-pi-cpp-parity-todo.md`. The pre-implementation cleanup in `docs/plans/2026-06-16-002-refactor-pre-implementation-cleanup-plan.md` established the structural prerequisites for larger parity slices: package-style CMake targets, CLI11 parsing, provider registry wiring with a registered fake provider, true async shell execution, expanded agent event/state seams, runtime service split, and parse-only session tree entry preparation.

## CLI states

The CLI prints stable semantic event lines:

- `[model-request] turn N`
- `[assistant] <text>`
- `[tool-call] <name>#<id>`
- `[tool-success] <id>`
- `[tool-error] <id>`
- `[provider-error] <message>`
- `[max-turns] max_turns_exceeded`
- `[completed] <stop reason>`

One-shot mode runs one prompt. `--repl` keeps history in memory for multiple prompts. `--resume <session.jsonl>` loads the redacted typed JSONL history and appends new messages. `--session <path>` always creates a new file; use `--resume` to append.

## Tools

The built-in tools are:

- `read_file`: read a text file inside the workspace, with optional line offset/limit.
- `write_file`: create or overwrite a file inside the workspace after parent validation.
- `edit_file`: perform one exact `old_text` / `new_text` replacement; zero or multiple matches are rejected.
- `bash`: run a shell command inside the workspace only when `--enable-bash` is passed.

The registry owns tool capabilities directly. File tools deliberately share one execution environment capability, which owns workspace containment, path validation, atomic writes, process execution, timeout handling, and output limiting. File tools reject workspace escapes, symlink escapes, directory/file mismatches, and missing parents unless creation is explicitly requested. `bash` receives a sanitized environment that omits API-key, token, secret, password, and OpenAI-looking variables.

## Sessions and safety

Sessions are JSONL:

1. a v2 `header` line with session/workspace/provider/model metadata,
2. append-only typed `message` entries containing redacted user, assistant, and tool-result messages,
3. parse-only support for pi-style v3 tree metadata entries such as `model_change`, `thinking_level_change`, `compaction`, `branch_summary`, `custom`, `custom_message`, `label`, `session_info`, and `leaf`,
4. safely ignored unknown future entry types.

The redacted v2 transcript is canonical for current resume/replay. Nontrivial tree sessions are refused for append/resume until tree context reconstruction lands, avoiding silent reconstruction of the wrong conversation state. Exact unredacted replay is intentionally out of scope. Session files are still sensitive: they can contain source text, command output, workspace paths, and provider/model metadata.

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
./build/cpp_harness_tests "[cli]"
```

These cover:

- public headers compile from the include contract surface without `src`, legacy sync contracts, Boost.JSON, or raw Glaze generic values in domain contracts;
- fake model/tool loops route through provider-neutral value contracts and owned tool capabilities;
- move-only event sinks can capture unique state and propagate errors;
- provider-specific OpenAI/SSE/Glaze wire mapping stays isolated from the agent loop;
- JSONL resume reconstructs typed message ordering for the next request;
- CLI fake-provider smoke tests demonstrate the current event/subscriber path without compatibility-only flags.

## Deferred

Not included yet: rich TUI, extensions/skills, additional provider adapters and model catalog/config resolution, OAuth, session tree navigation/branching/compaction semantics, multi-replacement edits, native Windows shell process-tree termination semantics, tool execution streaming updates, subagents, MCP/RPC embedding, permission prompts, image generation behavior, C++26 reflection-generated schema, `std::execution` senders/receivers, ABI-stable binary distribution, or OS-level sandboxing.

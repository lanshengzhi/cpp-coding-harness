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

```bash
cmake -S . -B build
cmake --build build -j2
ctest --test-dir build --output-on-failure
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

## Architecture boundaries

The code is split into value contracts, capability seams, and implementation adapters:

- `include/cch/ai`: passive message/content/tool/context contracts and provider-neutral stream events.
- `include/cch/agent`: coroutine agent loop, lifecycle event values, move-only event sinks, async tool registry, and expected-style tool execution contracts.
- `include/cch/harness` and `include/cch/tools`: local execution environment and built-in tool factory capability seams.
- `include/cch/util`: project error/expected contracts, move-only callback vocabulary, passive `JsonValue`, and the Glaze-backed JSON adapter.
- `include/cch/ai/glaze` and `src/ai/glaze`: explicit serialization DTOs and conversion helpers.
- `src/...`: implementation files for provider transport, local filesystem/process behavior, CLI runtime, and session persistence.

The build publishes `include` as the public surface and keeps `src` private. Legacy synchronous tools, Boost.JSON contracts, `util::Result`, and duplicate `src` contract headers have been removed.

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
3. safely ignored future entry types so resume flows can survive additive format growth.

The redacted transcript is canonical for resume/replay. Exact unredacted replay is intentionally out of scope. Session files are still sensitive: they can contain source text, command output, workspace paths, and provider/model metadata.

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

Not included yet: rich TUI, extensions/skills, multi-provider registries, OAuth, session trees/branching/compaction, multi-replacement edits, native Windows shell semantics, parallel tool execution, subagents, MCP/RPC embedding, permission prompts, image/thinking content behavior, C++26 reflection-generated schema, `std::execution` senders/receivers, ABI-stable binary distribution, or OS-level sandboxing.

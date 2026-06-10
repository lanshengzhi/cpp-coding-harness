# C++ Coding Harness

A small C++23 coroutine/Glaze coding-agent harness that mirrors the core pi-style loop:

1. accept a prompt from CLI or REPL,
2. send ordered messages plus JSON Schema tool definitions to an OpenAI-compatible chat API,
3. execute local tools requested through `tool_calls`,
4. append tool-result messages with matching call IDs,
5. repeat until the assistant stops or the max-turn limit is reached,
6. persist the redacted canonical transcript as JSONL.

This is a learning and experimentation harness, not a production sandbox.

## Build

The project is CMake-based, requires a C++23-capable compiler, and is moving to a typed Glaze JSON contract layer on top of the existing Boost networking/process spine. CMake 3.20 or newer is expected so the configured C++23 standard is understood by the generator.

- Glaze for typed JSON serialization/deserialization
- Boost.JSON remains only for legacy code that is being removed by the coroutine/Glaze refactor
- Boost.Beast/Asio + OpenSSL for HTTPS transport
- Boost.Process for the process-execution boundary
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

The CLI uses the typed `include/cch/...` contracts, the awaitable agent loop, coroutine-compatible tools, Glaze JSONL sessions, and streaming assistant events. `--async` is accepted as a no-op compatibility flag because this stack is now the default.

Real-provider mode is OpenAI Chat Completions-compatible:

```bash
export OPENAI_API_KEY=...
./build/cpp_harness --model gpt-4.1-mini --session /tmp/cpp-real.jsonl "summarize README.md"
```

Use `--base-url` for compatible gateways that preserve the `/v1/chat/completions` contract.

## Architecture boundaries

The code is split into three primary seams that mirror pi's contracts while staying idiomatic C++:

- `include/cch/ai`: Glaze-backed message/content/tool/context contracts, SSE parsing, awaitable stream transport, and the streaming OpenAI-compatible client.
- `include/cch/agent`: coroutine agent loop, lifecycle events, async tool registry, and expected-style tool execution contracts.
- `include/cch/harness` and `include/cch/tools`: coroutine-compatible local execution environment and built-in tool factories.
- `src/...`: implementation files for the public headers. The old `src/llm` and `src/session` compatibility facades have been removed.

## CLI states

The CLI prints stable transcript lines:

- `[model-request] turn N`
- `[assistant] <text>`
- `[tool-call] <name>#<id>`
- `[tool-success] <id>`
- `[tool-error] <id>`
- `[provider-error] <message>`
- `[max-turns] max_turns_exceeded`
- `[completed] <stop reason>`

One-shot mode runs one prompt. `--repl` keeps history in memory for multiple prompts. `--resume <session.jsonl>` loads the redacted canonical JSONL history and appends new messages.

## Tools

The built-in tools are:

- `read_file`: read a text file inside the workspace, with optional line offset/limit.
- `write_file`: create or overwrite a file inside the workspace after parent validation.
- `edit_file`: perform one exact `old_text` / `new_text` replacement; zero or multiple matches are rejected.
- `bash`: run a shell command inside the workspace only when `--enable-bash` is passed.

File tools execute through the harness execution environment, which owns workspace containment, path validation, atomic writes, process execution, timeout handling, and output limiting. File tools reject workspace escapes, symlink escapes, directory/file mismatches, and missing parents unless creation is explicitly requested. `bash` receives a sanitized environment that omits API-key, token, secret, password, and OpenAI-looking variables.

## Sessions and safety

Sessions are JSONL:

1. a v1 `header` line with session/workspace/provider/model metadata,
2. append-only typed `message` entries containing redacted user, assistant, and tool-result messages,
3. safely ignored future entry types so older resume flows keep working as the format grows.

The harness session store exposes typed entries while preserving the legacy linear message history used by CLI resume. The redacted transcript is canonical for resume/replay. Exact unredacted replay is intentionally out of MVP scope. Session files are still sensitive: they can contain source text, command output, workspace paths, and provider/model metadata.

The workspace guard is not a sandbox. Prompts, file contents, and command outputs can be sent to the configured provider. Run this harness inside a VM/container if you need a real containment boundary.

## Acceptance examples as executable specs

The default test binary names the MVP behavior slices:

```bash
./build/cpp_harness_tests "[agent][u4][ae1]"
./build/cpp_harness_tests "[ai][u2]"
./build/cpp_harness_tests "[ai][provider][u3]"
./build/cpp_harness_tests "[agent][u4]"
./build/cpp_harness_tests "[harness][u5]"
./build/cpp_harness_tests "[harness][session][u6]"
./build/cpp_harness_tests "[cli][u6]"
```

These cover:

- AE1: fake model requests `read_file`; the loop appends a matching tool-result and sends a second model request.
- AE2: ambiguous `edit_file` replacements are rejected without mutation.
- AE3: bash timeout behavior is represented through a fake process runner without wall-clock sleeps.
- AE4: JSONL resume reconstructs message ordering for the next request.
- AE5: the fake-client walking skeleton compiles and passes without live provider access.
- AE6: CLI fake-provider smoke tests and this README document how messages, tools, sessions, and workspace boundaries compose.
- AE7: provider-specific OpenAI wire mapping is isolated under `src/ai/providers` while the agent loop uses provider-neutral AI contracts.
- Async coverage: `[ai][provider][stream][u4]`, `[agent][async][u5]`, `[tools][async][u6]`, and `[cli][async][u7]` exercise the coroutine/Glaze path without live network access.

## Deferred from MVP

Not included yet: rich TUI, extensions/skills, multi-provider registries, OAuth, session trees/branching/compaction, multi-replacement edits, native Windows shell semantics, parallel tool execution, subagents, MCP/RPC embedding, permission prompts, image/thinking content behavior, full legacy session removal, or OS-level sandboxing.

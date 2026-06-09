# C++ Coding Harness MVP

A small C++20 coding-agent harness that mirrors the core pi-style loop:

1. accept a prompt from CLI or REPL,
2. send ordered messages plus JSON Schema tool definitions to an OpenAI-compatible chat API,
3. execute local tools requested through `tool_calls`,
4. append tool-result messages with matching call IDs,
5. repeat until the assistant stops or the max-turn limit is reached,
6. persist the redacted canonical transcript as JSONL.

This is a learning and experimentation harness, not a production sandbox.

## Build

The project is CMake-based and uses a Boost-first dependency spine:

- Boost.JSON for JSON DOM/parsing/serialization
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

Real-provider mode is OpenAI Chat Completions-compatible:

```bash
export OPENAI_API_KEY=...
./build/cpp_harness --model gpt-4.1-mini --session /tmp/cpp-real.jsonl "summarize README.md"
```

Use `--base-url` for compatible gateways that preserve the `/v1/chat/completions` contract.

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

File tools reject workspace escapes, symlink escapes, directory/file mismatches, and missing parents unless creation is explicitly requested. `bash` receives a sanitized environment that omits API-key, token, secret, password, and OpenAI-looking variables.

## Sessions and safety

Sessions are JSONL:

1. a v1 `header` line with session/workspace/provider/model metadata,
2. append-only `message` entries containing redacted user, assistant, and tool-result messages.

The redacted transcript is canonical for resume/replay. Exact unredacted replay is intentionally out of MVP scope. Session files are still sensitive: they can contain source text, command output, workspace paths, and provider/model metadata.

The workspace guard is not a sandbox. Prompts, file contents, and command outputs can be sent to the configured provider. Run this harness inside a VM/container if you need a real containment boundary.

## Acceptance examples as executable specs

The default test binary names the MVP behavior slices:

```bash
./build/cpp_harness_tests "[agent][u4][ae1]"
./build/cpp_harness_tests "[tools][u3]"
./build/cpp_harness_tests "[session][u5]"
./build/cpp_harness_tests "[llm][u2]"
./build/cpp_harness_tests "[cli][u6]"
```

These cover:

- AE1: fake model requests `read_file`; the loop appends a matching tool-result and sends a second model request.
- AE2: ambiguous `edit_file` replacements are rejected without mutation.
- AE3: bash timeout behavior is represented through a fake process runner without wall-clock sleeps.
- AE4: JSONL resume reconstructs message ordering for the next request.
- AE5: the fake-client walking skeleton compiles and passes without live provider access.
- AE6: CLI fake-provider smoke tests and this README document how messages, tools, sessions, and workspace boundaries compose.

## Deferred from MVP

Not included yet: rich TUI, extensions/skills, multiple provider-specific adapters, streaming, OAuth, session trees/branching, multi-replacement edits, native Windows shell semantics, parallel tool execution, subagents, MCP/RPC embedding, permission prompts, or OS-level sandboxing.

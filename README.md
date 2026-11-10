# Pike

<p align="center">
  <b>English</b> | <a href="README.zh-CN.md">简体中文</a>
</p>

An experimental C++23 coding-agent runtime with a Native TUI, one-shot CLI, and idiomatic C++ interfaces. Built for high performance, reproducible builds, and strict architectural integrity.

> **Note**
> Pike is a learning and experimentation harness, not a production sandbox: prompts, selected files, and tool output may be sent to the configured model provider via the local gateway.

---

## Quickstart

### 1. Start the local model gateway
Pike delegates upstream model routing, catalog discovery, and credential management to the local [Magpie](docs/usage.md#models-and-authentication) gateway at `http://127.0.0.1:3425`. Start it before running Pike:

```bash
magpie serve
```

### 2. Launch the interactive Native TUI
Open an interactive coding agent session in your terminal:

```bash
build/release/pike
```

### 3. One-shot CLI & Multimodal Prompts
Run prompts directly from the command line or pipe from standard input:

```bash
# Print response directly to stdout
build/release/pike --print "Explain this codebase structure"

# Pipe context from stdin
cat error.log | build/release/pike --print "Diagnose this failure"

# Multimodal and file context (PNG/JPEG/GIF/WebP are automatically detected by content)
build/release/pike @screenshot.png @main.cpp "Analyze the UI bug against the implementation"

# Resume or continue past sessions
build/release/pike --continue "Continue where we left off"
build/release/pike --resume
```

---

## Key Features

- **Native Terminal UI (TUI)**: Fast, keyboard-driven terminal interface featuring built-in slash commands (`/model`, `/tree`, `/fork`, `/compact`, `/reload`), session navigation, and configurable themes and keybindings.
- **Content-Aware Multimodal Input**: Prefix any path with `@` (e.g. `@spec.pdf`, `@diagram.webp`). Images are auto-detected by byte signatures and resized safely; text files are cleanly wrapped into prompt context.
- **Session Trees & Non-Destructive Forking**: Tree-structured session history with non-destructive branch points (`/fork`), tree inspection (`/tree`), and resumable sessions (`--resume`, `--continue`, `--session`).
- **In-Editor User Bash**: Execute shell commands directly from the editor prompt (`!cmd` to record command output into conversation history; `!!cmd` to run without cluttering LLM context).
- **Gateway Model Routing**: Model selection (`--model <id>`) interfaces with Magpie for live catalog discovery, upstream load balancing, and account rotation without storing cloud API keys in session files.
- **Fail-Closed C++23 Core**: Implements a strict no-exceptions policy (`-fno-exceptions`), zero-overhead abstractions, and compile-time Parity Architecture Gate checks.

---

## Requirements

The Supported Platform is **native Linux x86-64 with glibc**.

- **Compiler**: GCC 16.x (Clang 22.x for debug conformance)
- **Build System**: CMake 4.4+, Ninja 1.11+
- **Package Manager**: Pinned vcpkg manifest (bootstrapped automatically; system-package dependencies are unsupported)

See [ADR 0038](docs/adr/0038-reproducible-build-toolchain-floor-gcc-16-cmake-4-single-vcpkg-path.md) and [ADR 0039](docs/adr/0039-own-the-capability-owner-package-graph-and-parity-architecture-gate.md) for toolchain details.

---

## Building and Testing

### Release Build and Test

The bootstrap script checks host tools, creates or reuses `.deps/vcpkg`, pins it to the `vcpkg.json` baseline, and builds the pinned vcpkg binary. CMake then handles configure, build, and test:

```bash
# 1. Bootstrap dependencies
scripts/bootstrap.sh

# 2. Build Release binary
cmake --preset vcpkg-release
cmake --build --preset vcpkg-release

# 3. Run validation test suite
ctest --preset vcpkg-release
```

The compiled Pike binary is located at `build/release/pike`.

### Verify the Build

```bash
build/release/pike --version
build/release/pike --help
```

*Note: Default tests are deterministic, use fake providers, and make no external network requests.*

### Install

Install the standalone runtime to your preferred prefix:

```bash
cmake --install build/release --prefix ~/.local
~/.local/bin/pike --version
```

---

## Development Loop

Day-to-day development uses the default `vcpkg` Debug preset (`build/` tree):

```bash
cmake --preset vcpkg               # Run once per checkout
cmake --build --preset vcpkg       # Incremental build
ctest --preset vcpkg               # Full offline test suite
```

### Focused Shard Testing
During development, speed up iteration by building only the target module and excluding the full architecture gate:

```bash
cmake --build --preset vcpkg --target cch_tests_coding_agent
ctest --preset vcpkg -LE architecture -L coding_agent
```

### Sanitizer Builds

Sanitizers run against isolated fake providers with no live network dependencies:

```bash
# AddressSanitizer & UndefinedBehaviorSanitizer
cmake --preset vcpkg-asan-ubsan && cmake --build --preset vcpkg-asan-ubsan
ctest --preset vcpkg-asan-ubsan

# ThreadSanitizer (TSan) asynchronous scenarios
cmake --preset vcpkg-tsan && cmake --build --preset vcpkg-tsan
scripts/ci/tsan-scenarios.sh
```

---

## Documentation

- 📖 [Usage Guide](docs/usage.md) — Comprehensive guide to CLI flags, session management, configuration, TUI slash commands, and User Bash.
- ⌨️ [Keybindings](docs/keybindings.md) — Native TUI keybinding grammar, action maps, and custom bindings.
- 🔄 [Agent Lifecycle](docs/agent-lifecycle.md) — Detailed event loop ordering, execution phases, and cancellation rules.
- 🏗️ [Architecture Guide](docs/agents/architecture.md) — Owner packages, seam boundaries, and ADR index.
- 🧪 [Validation & Delivery](docs/agents/validation.md) — Focused/Full/Fresh validation tiers, strict no-exception policy, and CI release qualification matrix.
- 📚 [Glossary](GLOSSARY.md) — Canonical definitions and domain vocabulary.

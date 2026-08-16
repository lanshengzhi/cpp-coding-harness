# C++ Coding Harness

An experimental C++23 coding-agent Runtime that preserves a selected pi capability set with idiomatic C++ interfaces. It is a learning and experimentation harness, not a production sandbox: prompts, selected files, and tool output may be sent to the configured model provider.

## Requirements

The Supported Platform is native Linux x86-64 with glibc. Builds require GCC 16.x, CMake 4.4+, Ninja 1.11+, and the pinned vcpkg manifest path; system-package dependency builds are unsupported. See [ADR 0038](docs/adr/0038-reproducible-build-toolchain-floor-gcc-16-cmake-4-single-vcpkg-path.md) and [ADR 0039](docs/adr/0039-own-the-capability-owner-package-graph-and-parity-architecture-gate.md) for the toolchain and release boundary.

## Release build and test

The bootstrap script checks the host tools, creates or reuses `.deps/vcpkg`, pins it to `vcpkg.json`, configures the Release preset, builds, and runs the offline test suite:

```bash
scripts/bootstrap.sh --release --test
```

Useful variants:

```bash
scripts/bootstrap.sh --test                    # Debug build and tests
scripts/bootstrap.sh --release --no-build      # Release configure only
scripts/bootstrap.sh --vcpkg-root /path/to/vcpkg --release --test
```

With an already bootstrapped checkout at the exact `vcpkg.json` baseline:

```bash
export VCPKG_ROOT=/path/to/vcpkg
cmake --preset vcpkg-release
cmake --build --preset vcpkg-release
ctest --preset vcpkg-release
```

The Release executable is `build/release/cpp_harness`. Run the unfiltered CTest preset for final validation.

## Sanitizer builds

Two blocking CI jobs (issue #473) add sanitizer coverage on top of the default matrix:

- `vcpkg-asan-ubsan` — Debug build with AddressSanitizer and UndefinedBehaviorSanitizer (`CCH_SANITIZER=address;undefined`), running the full supported test suite with symbolized, halting reports.
- `vcpkg-tsan` — Debug build with ThreadSanitizer (`CCH_SANITIZER=thread`), running `scripts/ci/tsan-scenarios.sh`: focused repeated scenarios over the asynchronous ownership and shutdown seams (`AsyncResult`, mailboxes, subscriptions, process draining, persistence, cancellation, Session replacement, and Session Close) within bounded repetition and timeout budgets. Each run prints the scenario, label selection, budget, git revision, and sanitizer options needed to reproduce a finding.

Both jobs use fake providers and isolated temporary resources with blanked credentials — no live-provider or network dependency, same as the default suite.

```bash
cmake --preset vcpkg-asan-ubsan && cmake --build --preset vcpkg-asan-ubsan
ctest --preset vcpkg-asan-ubsan

cmake --preset vcpkg-tsan && cmake --build --preset vcpkg-tsan
scripts/ci/tsan-scenarios.sh
```

Point `ASAN_SYMBOLIZER_PATH` (and `external_symbolizer_path` in `TSAN_OPTIONS`) at an `llvm-symbolizer` binary for symbolized reports.

Every supported configure runs the Parity Architecture Gate fail-closed (ADR 0039), including tests-disabled configurations; every normal build, direct production-target build, and CTest entry point additionally requires fresh successful build-phase Gate evidence (compile commands and compiler depfiles). The deterministic machine-readable report is written to `<binary-dir>/parity-build-gate.json` (for the default preset, `build/parity-build-gate.json`) on every run, pass or fail.

## Install

`cmake --install` installs the Runtime-only surface approved in [ADR 0039](docs/adr/0039-own-the-capability-owner-package-graph-and-parity-architecture-gate.md): the `cpp_harness` executable under `bin/` and the required third-party license/notice texts under `share/cpp_harness/licenses/`. No Owner Interface headers, static libraries, CMake package metadata, exported targets, or other development surface are installed; the install tree is intentionally unsuitable for external C++ consumers.

```bash
cmake --install build/release --prefix ~/.local
~/.local/bin/cpp_harness --version
```

The install path runs the fail-closed install gate before any file is staged: the build-phase Parity Architecture Gate, a Gate-evidence freshness check (a source or header edit without a rebuild fails the install), and a dependency-closure audit that rejects undeclared, build-tree, or unsupported runtime dependencies (cmake/install/). There is no opt-out.

The release seam is validated from a clean staging prefix by the `cch_install_tools_unit` and `cch_install_gate_fixture` CTest cases and the `[cli][install]` relocation case (`tests/cli/InstallRelocationTest.cpp`), which covers the staged layout, dependency closure, `--help`, `--version`, and an offline fake-provider smoke run outside the build tree.

## Verify the build

```bash
build/release/cpp_harness --version
build/release/cpp_harness --help
```

Default tests are deterministic, use fake Providers, and make no live-provider requests.

## Usage

See [docs/usage.md](docs/usage.md) for prompts, files and images, model authentication, sessions, configuration, Native TUI commands, and User Bash.

Project terminology lives in [CONTEXT.md](CONTEXT.md), keybinding details in [docs/keybindings.md](docs/keybindings.md), and architecture decisions in [docs/adr/](docs/adr/).

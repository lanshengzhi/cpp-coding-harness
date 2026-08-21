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
scripts/bootstrap.sh --test                    # Fresh Validation (environment level): Debug bootstrap, --fresh configure, build, and tests
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

## Debug development loop

Day-to-day development uses the default `vcpkg` preset (Debug, `build/` tree). With an already bootstrapped checkout at the exact `vcpkg.json` baseline:

```bash
export VCPKG_ROOT=/path/to/vcpkg   # needed for the configure step
cmake --preset vcpkg               # once per checkout; no --fresh for incremental work
cmake --build --preset vcpkg       # incremental build
ctest --preset vcpkg               # Full Validation: the complete unfiltered offline suite
```

For during-implementation Focused Validation, `scripts/check.sh` wraps this loop: incremental build, CTest name/label selection passed straight through (`-R`, `-L`, ...), the architecture-labeled whole-graph gate tests excluded by default, `--architecture` selecting them, and `--target <shard>` narrowing the build. See [docs/agents/validation.md](docs/agents/validation.md).

`scripts/bootstrap.sh --test` is Fresh Validation, the environment-level tier (vcpkg bootstrap, `--fresh` configure, full build, unfiltered suite); reserve it for clean checkouts, vcpkg-baseline or toolchain changes, configure-orchestration changes, or explicit request.

The `dev-fast` preset is an optional local accelerator for the Debug loop and requires ccache; the supported default remains the `vcpkg` preset.

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

## Release qualification (CI)

Issue #474 establishes the blocking release matrix in `.github/workflows/linux-toolchain.yml`:

- **GCC 16.x Debug and Release** jobs build and test the complete supported product on Ubuntu 24.04; **Clang 22.x** is a blocking Debug conformance verifier over the same graph and suite and never produces release artifacts.
- **GCC 16 Release artifact (IPO/LTO)** — the `vcpkg-release-artifact` preset builds the release Runtime with validated IPO/LTO (configure fails closed if the toolchain cannot do IPO). The job stages it into a clean prefix through the fail-closed install gate, audits the staged dependency closure, runs `scripts/ci/release-artifact-smoke.sh` (relocation, scrubbed-environment `--version`/`--help`, deterministic offline failure), binds the evidence to the artifact digest, and verifies the complete evidence directory with `scripts/ci/verify_release_evidence.py`. Missing, stale, contradictory, or incomplete evidence fails qualification with stable `REL-*` rule identifiers; nothing degrades to a warning. The qualified staged tree and its evidence upload as the `cpp_harness-runtime-linux-x86_64` run artifact.
- **Arch pinned (blocking)** — a digest-pinned `archlinux:base-devel` snapshot with pacman pinned to the matching Arch Linux Archive date runs the same declared dependency (pinned vcpkg) and Gate contracts. Toolchain drift (e.g. GCC leaving 16.x) fails the lane closed and forces a deliberate re-pin.
- **Arch latest drift (advisory)** — `.github/workflows/arch-drift.yml` runs current Arch latest on a weekly schedule. It never triggers on pull requests or pushes, so it cannot gate qualification or substitute for the pinned blocking lane; a failure is the drift signal to re-pin.

The evidence verifier is covered by the `cch_release_evidence_unit` CTest case (`tests/install/release_evidence_test.py`).

## Verify the build

```bash
build/release/cpp_harness --version
build/release/cpp_harness --help
```

Default tests are deterministic, use fake Providers, and make no live-provider requests.

## Usage

See [docs/usage.md](docs/usage.md) for prompts, files and images, model authentication, sessions, configuration, Native TUI commands, and User Bash.

Project terminology lives in [CONTEXT.md](CONTEXT.md), keybinding details in [docs/keybindings.md](docs/keybindings.md), and architecture decisions in [docs/adr/](docs/adr/).

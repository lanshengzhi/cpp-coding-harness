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

## Install

A supported Runtime-only install is approved in [ADR 0039](docs/adr/0039-own-the-capability-owner-package-graph-and-parity-architecture-gate.md) but is not implemented yet. The current CMake project has no install rules, so `cmake --install` installs no files. Issue [#472](https://github.com/lanshengzhi/cpp-coding-harness/issues/472) tracks the relocatable Runtime, required resources and notices, dependency audit, and clean-prefix smoke validation.

After #472 lands, the intended user location is `~/.local/bin/cpp_harness`. Until then, use the executable from the build tree rather than treating a manual copy as a supported installation.

## Verify the build

```bash
build/release/cpp_harness --version
build/release/cpp_harness --help
```

Default tests are deterministic, use fake Providers, and make no live-provider requests.

## Usage

See [docs/usage.md](docs/usage.md) for prompts, files and images, model authentication, sessions, configuration, Native TUI commands, and User Bash.

Project terminology lives in [CONTEXT.md](CONTEXT.md), keybinding details in [docs/keybindings.md](docs/keybindings.md), and architecture decisions in [docs/adr/](docs/adr/).

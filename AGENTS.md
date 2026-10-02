# C++ Coding Harness

This is an experimental C++23 coding-agent Runtime that preserves selected pi semantics behind strict Owner-package and safety boundaries.

## Start gate

1. Inspect `git status --short`. Treat every pre-existing modified or untracked file as user-owned; preserve unrelated work.
2. Fetch any named GitHub issue or PRD, then read only the task-specific context linked below.
3. Stop exploring once you can name the behavior, authoritative seam, constraints, and validation path.
4. Settle a technical direction only after the smallest probe measures its cost.

## Validation entry points

Read [validation](docs/agents/validation.md) before implementation, review, or delivery. It owns the Focused, Full, and Fresh Validation tiers, their commands, and the documentation-only exception.

## Task-specific context

- **Implementation:** read [validation](docs/agents/validation.md); consult [CODING_STANDARDS.md](CODING_STANDARDS.md) §2 (mechanical style) and §11 (tests) as the change requires.
- **Review:** read [CODING_STANDARDS.md](CODING_STANDARDS.md) and [validation](docs/agents/validation.md).
- **Architecture, Owner packages, module structure, capability seams, or security boundaries:** read [architecture](docs/agents/architecture.md), [pi parity](docs/agents/pi-parity.md), and the relevant accepted ADRs.
- **pi-ai catalog or upstream baseline sync:** read `fixtures/pi-ai/README.md` and the module's accepted ADR.
- **Running or E2E-testing the product:** read [usage](docs/usage.md); the TUI seams and their fixed-width conventions live in `tests/coding_agent/tui/`.

## Agent skills

### Issue tracker

Issues and specs live in GitHub Issues. See `docs/agents/issue-tracker.md`.

### Triage labels

The tracker uses the canonical triage labels. See `docs/agents/triage-labels.md`.

### Domain docs

**Domain language or behavior contracts:** read [domain docs](docs/agents/domain.md) for this single context's `GLOSSARY.md`, topic-specific contract references, and ADR conflict rules.

## Cursor Cloud specific instructions

Day-to-day commands stay the Debug `vcpkg` preset from the README: `scripts/bootstrap.sh`, `cmake --preset vcpkg`, `cmake --build --preset vcpkg`, and `ctest --preset vcpkg`. Default tests are offline and use fake providers.

This VM has no swap. Keep port builds at `VCPKG_MAX_CONCURRENCY=2` and pass `--parallel 2` to `cmake --build`. The environment install script exports that cap and a binary cache at `/var/cache/vcpkg-archives`.

GCC 16.0.1 is the `gcc` and `g++` on PATH (PPA package `16-20260315-1ubuntu1~24~ppa1`). CMake 4.4.2 and Ninja 1.13.2 are `/usr/local/bin/cmake` and `/usr/local/bin/ninja`. Keep the `cc` and `c++` alternatives on those GCC 16 drivers. vcpkg port builds follow `CC` and `CXX`, which the install script sets to `gcc-16` and `g++-16`.

A product smoke check is `build/pike --version`, `build/pike --help`, and `build/pike --print ping` with an empty environment. The print command exits non-zero and prints `Unknown provider: unknown`.

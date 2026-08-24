# C++ Coding Harness

This is an experimental C++23 coding-agent Runtime that preserves selected pi semantics behind strict Owner-package and safety boundaries.

## Start gate

1. Inspect `git status --short`. Treat every pre-existing modified or untracked file as user-owned; preserve unrelated work.
2. Fetch any named GitHub issue or PRD, then read only the task-specific context linked below.
3. Stop exploring once you can name the behavior, authoritative seam, constraints, and validation path.

## Validation entry points

Dependencies use the pinned vcpkg manifest; system packages are unsupported. Three tiers (see `CONTEXT.md`):

- **Focused Validation** — during implementation: `cmake --build --preset vcpkg --target <owning-shard>` to narrow the build, followed by `ctest --preset vcpkg -LE architecture -R '<name>'` (or `-L '<label>'`) for the smallest CTest selection that can fail. Architecture-sensitive changes additionally run `ctest --preset vcpkg -L architecture`.
- **Full Validation** — once before delivery: incremental `cmake --build --preset vcpkg` followed by the complete unfiltered `ctest --preset vcpkg` (see [README.md](README.md)).
- **Fresh Validation** — environment level: `scripts/bootstrap.sh` (host precheck plus pinned vcpkg), then `export VCPKG_ROOT="$PWD/.deps/vcpkg"`, `cmake --preset vcpkg --fresh`, `cmake --build --preset vcpkg`, and `ctest --preset vcpkg`; reserved for clean checkouts, vcpkg-baseline or toolchain changes, configure-orchestration changes, or explicit user request.

Do not run Fresh Validation for ordinary code edits.

## Task-specific context

- **Implementation or review:** read [CODING_STANDARDS.md](CODING_STANDARDS.md) and [validation](docs/agents/validation.md).
- **Architecture, Owner Interface headers, dependency direction, capability seams, or security boundaries:** read [architecture](docs/agents/architecture.md) and the relevant accepted ADRs.
- **pi parity:** read [pi parity](docs/agents/pi-parity.md), then inspect the relevant current pi source or documentation.
- **Domain language:** read [domain docs](docs/agents/domain.md), `CONTEXT.md`, and relevant accepted ADRs.

## Agent skills

### Issue tracker

Issues and specs live in GitHub Issues. See `docs/agents/issue-tracker.md`.

### Triage labels

The tracker uses the canonical triage labels. See `docs/agents/triage-labels.md`.

### Domain docs

This is a single-context repository. See `docs/agents/domain.md` for how to consume `CONTEXT.md` and `docs/adr/`.

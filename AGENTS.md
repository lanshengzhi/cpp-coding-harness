# C++ Coding Harness

This is an experimental C++23 coding-agent Runtime that preserves selected pi semantics behind strict Owner-package and safety boundaries.

## Start gate

1. Inspect `git status --short`. Treat every pre-existing modified or untracked file as user-owned; preserve unrelated work.
2. Fetch any named GitHub issue or PRD, then read only the task-specific context linked below.
3. Stop exploring once you can name the behavior, authoritative seam, constraints, and validation path.

## Build entry point

Dependencies use the pinned vcpkg manifest; system packages are unsupported. Use `scripts/bootstrap.sh --test` for a fresh full Debug validation. Read [README.md](README.md) for Release and manual preset commands.

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

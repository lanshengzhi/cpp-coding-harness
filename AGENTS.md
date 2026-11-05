# C++ Coding Harness

Experimental C++23 coding-agent runtime with strict Owner packages and safety boundaries.

## Start gate

1. Inspect `git status --short`. User owns pre-existing modified or untracked files; preserve unrelated work.
2. Fetch named issue or PRD (`gh issue view <n> --comments`); read only the task's context below.
3. Stop exploring once behavior, authoritative seam, constraints, and validation path are named.
4. Settle technical directions only after the smallest probe measures cost.

## Validation entry points

Read [validation](docs/agents/validation.md) for Focused, Full, and Fresh tiers, their commands, and the docs-only exception.

## 并发约束 (Concurrency)

One code lane at a time in the main checkout (owner rule, 2026-11-05; lane definitions and incident record: [ops-lanes](docs/wiki/pi-alignment/ops-lanes.md)).

1. Before starting any build/test/evidence work (`cmake --build`, `ctest`, `consolidate_audits.py`, the ledger or delivery files), inspect `git status --short` for in-flight lanes and run `scripts/dev/lane-lock.sh status`.
2. Take the checkout explicitly: `scripts/dev/lane-lock.sh acquire <lane> [timeout]`; release with `release <lane>` (fail-closed — only the holder releases). The consolidation `build`/`check` refuse to run under a foreign `.lane-lock.json`: the caller proves its lane with `CCH_LANE_NAME`, and only the owning lane sets `CCH_LANE_LOCK_SKIP=1`.
3. 每个 lane 完成后立即 push，下个 lane 从新的 HEAD 开始（serial-merger handoff）。
4. Light tasks — `gh` operations, `docs/wiki/` edits, read-only research — may run in parallel with a code lane; two code lanes never overlap, in any session.

## Task-specific context

- **Implementation:** read [validation](docs/agents/validation.md); check [CODING_STANDARDS.md](CODING_STANDARDS.md) §2 (mechanical style) and §11 (tests).
- **Repeated mechanical work:** do one instance by hand; build and check the smallest re-runnable tool.
- **Review:** read [CODING_STANDARDS.md](CODING_STANDARDS.md) and [validation](docs/agents/validation.md).
- **Architecture, Owner packages, module structure, seams, or security boundaries:** read [architecture](docs/agents/architecture.md), [pi parity](docs/agents/pi-parity.md), and relevant ADRs.
- **pi-ai catalog or upstream sync:** read `fixtures/pi-ai/README.md` and the module's ADR.
- **Running or E2E tests:** read [usage](docs/usage.md); TUI seams live in `tests/coding_agent/tui/`. To drive CLI/TUI and capture evidence, read `.agents/skills/verify-pike/SKILL.md`.

## Agent skills

- **Issue tracker:** GitHub Issues via `gh`. See [issue-tracker](docs/agents/issue-tracker.md).
- **Triage labels:** canonical roles to tracker labels. See [triage-labels](docs/agents/triage-labels.md).
- **Domain language or behavior contracts:** read [domain docs](docs/agents/domain.md) for [GLOSSARY.md](GLOSSARY.md), topic contracts, and ADR conflict rules.

# AGENTS.md

This file is the **RootRouter**: the entry point for every agent run. Its job is to route you to the right context, guardrails, and validation slice with minimal reading.

**How to use this file:** follow the three-step loop — **Start Gate** → **Route** → **Verify** — then hand off. Each section states its purpose and completion criterion so you know when to stop reading.

---

## Start Gate

**Purpose:** protect user work and classify the task before reading broadly.

**Completion criterion:** you know the task family and which reference file is authoritative for it.

1. Inspect the working tree:

   ```bash
   git status --short
   ```

2. Protect user work. Treat every pre-existing modified or untracked file as user-owned unless the task explicitly targets it. Do not overwrite, reformat, delete, or “clean up” unrelated changes. If a target file already has changes, inspect enough diff/context to preserve them and edit only the requested scope.

3. Classify the task:
   - **Docs/route/issue maintenance:** read the referenced issue/PRD and the affected docs only.
   - **Implementation or bug fix:** read `README.md` for behavior/safety context, `CMakeLists.txt` for targets, then the matching route row in `docs/agents/module-routing.md`.
   - **Architecture, public contracts, include boundaries, or pi parity:** read `CONTEXT.md` for domain terms, discover active plans with `status: active` under `docs/plans/`, read the relevant plan(s), and read the matching `pi:` reference files or docs before designing changes.

4. Select the smallest useful context. Stop initial exploration when you know the task family, authoritative route reference, safety constraints, and validation slice.

---

## Guardrails

**Purpose:** always-on architecture rules. Every change must preserve them.

**Completion criterion:** your change does not reintroduce a forbidden pattern and does not violate any rule below.

1. **Data is passive value state.** Public contracts use aggregate-friendly `struct`, `std::variant`, `std::expected`, and project `util::JsonValue` values.
2. **Capabilities cross physical seams.** Chat clients, stream transports, execution environments, session stores, and tools are exposed through interfaces or dependency-heavy concrete implementations hidden behind narrow headers.
3. **Events are weak connections.** Agent/provider event sinks use `std::move_only_function`; do not regress to copyability requirements such as `std::function`.
4. **Generic and serialization machinery stays local.** Glaze DTOs, schema conversion, visitors, parsing helpers, and similar machinery stay in serialization/implementation layers, not domain-facing APIs.
5. **Security and containment remain explicit.** Any shell, file, environment-variable, provider, or session change must preserve workspace containment, secret redaction, output truncation, and the documented “not a sandbox” boundary.

**Forbidden regressions:** do not reintroduce the legacy synchronous tool surface, `util::Result`, Boost.JSON domain contracts, `src` as a public include surface, or compatibility-only empty flags.

> **Why these rules?** They encode the anti-fragile architecture decision: passive values at public seams, replaceable adapters behind narrow headers, move-only callbacks, and serialization kept private. For the full domain vocabulary see `CONTEXT.md`.

---

## pi C++ Parity Direction

**Purpose:** state the long-term strategic direction so you can judge whether a change touches a parity area.

**Completion criterion:** you know whether the changed seam is part of the pi C++ parity roadmap, and if so, which active plan governs it.

The long-term direction is for this repository to become an idiomatic C++ implementation of pi’s module and contract architecture. Prefer module/contract parity over mechanical TypeScript translation.

- Roadmap: `docs/plans/2026-06-16-001-refactor-pi-cpp-parity-todo.md`.
- Contract inventory: `docs/plans/2026-06-16-003-refactor-pi-cpp-contract-inventory.md`.
- Current implementation plans: files under `docs/plans/` with `status: active`.
- Reference paths from the pi repository use the `pi:` prefix, for example `pi:packages/ai/src/types.ts`; this repository’s paths stay repo-relative.

Before changing a parity area, read the relevant roadmap/inventory entry and the referenced `pi:` contract or documentation.

---

## Route

**Purpose:** find the right entry point and authoritative reference for your task family.

**Completion criterion:** you have opened the file(s) listed in the matching route row.

Use the compact table below for a first guess; read [`docs/agents/module-routing.md`](docs/agents/module-routing.md) when the target seam is unclear, when you are editing routing docs, or when a task touches provider, tool, session, CLI/runtime, public-boundary, documentation, or pi-parity details. The detailed reference also carries the **validation slice** for each task family.

| Task family | Continue with |
| --- | --- |
| Agent loop, lifecycle events, tool-call orchestration | `docs/agents/module-routing.md` → Agent loop row |
| AI contracts, content, usage, provider-neutral messages | `docs/agents/module-routing.md` → AI messages/contracts row |
| Providers, OpenAI-compatible transport, SSE, model registry | `docs/agents/module-routing.md` → provider rows |
| Built-in tools, workspace/file/shell capabilities | `docs/agents/module-routing.md` → built-in tools and workspace/path/shell rows |
| CLI, REPL, JSON/RPC modes, runtime services, prompt processing | `docs/agents/module-routing.md` → CLI/runtime, JSON/RPC, and prompt rows |
| Config, project trust, resources, skills, prompt templates | `docs/agents/module-routing.md` → config, project trust, skills/resources rows |
| Sessions, resume, tree navigation, compaction context | `docs/agents/module-routing.md` → session row |
| Public headers, dependency direction, architecture guards | `docs/agents/module-routing.md` → public boundary/architecture row |
| Documentation, plans, issue tracker, route maintenance | The referenced issue/PRD and affected docs in this file, `docs/agents/module-routing.md`, `docs/plans/`, or `.scratch/<feature-slug>/` |

Condensed or removed detailed route content from the old root document is preserved in `docs/agents/module-routing.md`. Historical references to old `AGENTS.md` section numbers are mapped there. Build/test command examples remain in `README.md`; source/target membership remains authoritative in `CMakeLists.txt`.

---

## Verify Slice

**Purpose:** choose the smallest validation that proves the changed seam, and no more.

**Completion criterion:** you know exactly which checks to run for this task, and why larger checks are unnecessary.

- **Docs-only changes:** check markdown structure, relative links, headings, and no-information-loss against the referenced source (`docs/agents/module-routing.md`, the issue/PRD, or the old section being condensed). For agent-facing docs, also verify clear English optimized for agent execution. No C++ build is required unless code/build files changed.
- **Implementation changes:** run the focused test tag or executable slice listed in `README.md` and the relevant route row.
- **Public-boundary or contract changes:** if you change public headers, include surfaces, dependency directions, provider/tool/session contracts, or CMake public/private target boundaries, run the architecture tests before handoff.
- **Provider changes requiring real API keys or network:** do not run live provider validation by default; use fake/provider unit tests unless the user explicitly asks for live validation.

Tests should protect current architecture intent and safety properties, not only old class names, old JSONL shapes, or transcript wording.

> **Why smallest-first?** Kent Beck’s rule applies here: run the cheapest test that can fail for the change you made. Escalate only when a cheaper slice cannot give you confidence.

---

## Worktree Discipline

**Purpose:** keep changes minimal, user-owned, and safely reversible.

**Completion criterion:** only task-related files are modified, and pre-existing user changes are preserved.

- Edit the minimum file set needed for the task.
- Prefer a feature branch for extended work, but do not commit, merge, push, delete branches, or force-push unless the user explicitly asks.
- Detailed branch, merge, publish, PR, and cleanup conventions live in `docs/agents/worktree-discipline.md`; read it only when the task asks for those operations.
- Before final response, re-check `git status --short` and ensure listed changes are task-related.

---

## Handoff

**Purpose:** close the loop with clear accountability.

**Completion criterion:** the user can answer three questions from your response: what changed, how was it validated, and what was skipped.

Final responses must include:

- **Changed scope:** the files or seams changed and why they were in scope.
- **Validation performed:** tests, markdown checks, link checks, or no-information-loss checks run.
- **Skipped validation:** any intentionally skipped validation and the reason.

---

## Agent skills and references

**Purpose:** point to supporting conventions without inlining them.

- **Issue tracker:** Issues and PRDs live as markdown files in `.scratch/<feature-slug>/`. See `docs/agents/issue-tracker.md`.
- **Triage labels:** Default mattpocock/skills vocabulary: `needs-triage`, `needs-info`, `ready-for-agent`, `ready-for-human`, `wontfix`. See `docs/agents/triage-labels.md`.
- **Domain docs:** Single-context repo: read root `CONTEXT.md` plus `docs/adr/` (when present). See `docs/agents/domain.md`.
- **Worktree discipline:** `docs/agents/worktree-discipline.md` (branch, merge, publish, PR, and cleanup conventions).

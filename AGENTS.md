# AGENTS.md

This file is the **RootRouter** for every agent run. Use it to reach the smallest authoritative context, preserve user work, select validation, and hand off clearly.

## Start Gate

**Purpose:** protect user work and classify the task before reading broadly.

1. Inspect the working tree:

   ```bash
   git status --short
   ```

2. Treat every pre-existing modified or untracked file as user-owned unless the task explicitly targets it. Do not overwrite, reformat, delete, or clean unrelated changes.
3. Classify the task and read only its entry context:
   - **Docs, tracker, or route maintenance:** read the referenced spec/ticket and affected docs.
   - **Implementation or bug fix:** read `README.md`, `CMakeLists.txt`, and the matching row in `docs/agents/module-routing.md`.
   - **Architecture or public contracts:** read `CONTEXT.md`, relevant accepted ADRs under `docs/adr/`, and the matching route row.
   - **pi parity:** also read `.scratch/pi-cpp-parity/map.md` at low resolution and the relevant current `pi:` contract or documentation. Read individual map tickets only when selected.
4. Stop initial exploration when the task family, authoritative seam, safety constraints, and validation slice are known. Do not scan implemented tracker records by default.

## Guardrails

1. **Data is passive value state.** Public contracts use aggregate-friendly `struct`, `std::variant`, `std::expected`, and project `util::JsonValue` values.
2. **Capabilities cross physical seams.** Chat clients, stream transports, execution environments, session stores, and tools are exposed through interfaces or dependency-heavy implementations hidden behind narrow headers.
3. **Events are weak connections.** Agent/provider event sinks use `std::move_only_function`; do not regress to copyability requirements such as `std::function`.
4. **Generic and serialization machinery stays local.** Glaze DTOs, schema conversion, visitors, parsing helpers, and similar machinery stay in serialization or implementation layers.
5. **Security and containment remain explicit.** Shell, file, environment-variable, provider, and session changes must preserve workspace containment, secret redaction, output truncation, and the documented “not a sandbox” boundary.

Do not reintroduce the legacy synchronous tool surface, `util::Result`, Boost.JSON domain contracts, `src` as a public include surface, or compatibility-only empty flags.

## pi C++ Parity Direction

The long-term direction is an idiomatic C++ implementation of pi's module and contract architecture, not a mechanical TypeScript translation.

- The current planning authority is `.scratch/pi-cpp-parity/map.md`.
- Stable current behavior belongs in code, tests, `README.md`, and `docs/agents/module-routing.md`.
- Hard-to-reverse decisions belong in accepted ADRs.
- A decided feature leaves the map and follows `/to-spec` → `/to-tickets` → `/implement` as its own effort.
- Reference paths from pi use the `pi:` prefix, for example `pi:packages/ai/src/types.ts`.

## Route

Use the compact table for dispatch. Open `docs/agents/module-routing.md` for the detailed entry points and validation slice.

| Task family | Continue with |
| --- | --- |
| Agent loop, lifecycle events, tool-call orchestration | Agent loop row |
| AI contracts, content, usage, provider-neutral messages | AI messages/contracts row |
| Providers, OpenAI-compatible transport, SSE, model registry | Provider rows |
| Built-in tools, workspace/file/shell capabilities | Built-in tools and workspace/path/shell rows |
| CLI, REPL, JSON/RPC modes, runtime services, prompt processing | CLI/runtime, JSON/RPC, and prompt rows |
| Config, project trust, resources, skills, prompt templates | Agent config directory/user settings, project trust, and skills/resources rows |
| Sessions, resume, tree navigation, compaction context | Session row |
| Public headers, dependency direction, architecture guards | Public boundary/architecture row |
| Documentation and tracker maintenance | Referenced spec/ticket plus affected `README.md`, `CONTEXT.md`, `docs/agents/`, or `docs/adr/` |

Build and test commands remain authoritative in `README.md`; target membership remains authoritative in `CMakeLists.txt`.

## Verify Slice

- **Docs-only changes:** check Markdown structure, relative links, headings, tracker-state consistency, clear agent-facing English, and no-information-loss for migrated current facts. No C++ build is required unless code or build files changed.
- **Implementation changes:** run the focused test tag or executable slice listed in `README.md` and the relevant route row.
- **Public-boundary or contract changes:** run architecture tests when public headers, include surfaces, dependency directions, provider/tool/session contracts, or CMake public/private boundaries change.
- **Provider changes requiring real API keys or network:** use fake/provider unit tests unless the user explicitly asks for live validation.

Use the cheapest check that can fail for the changed seam, then escalate only when needed.

## Worktree Discipline

- Edit the minimum task-related file set.
- Prefer a feature branch for extended work, but do not commit, merge, push, delete branches, or force-push unless the user explicitly authorizes that operation.
- Invoking `/implement` is explicit authorization for its native task-scoped review and commit on the current branch. It does not authorize creating or switching branches/worktrees, pushing, merging, deleting branches, or including unrelated changes.
- Detailed branch and publishing conventions live in `docs/agents/worktree-discipline.md`; read them only when those operations are requested.
- Re-check `git status --short` before handoff.

## Handoff

Final responses must state:

- **Changed scope:** files or seams changed and why.
- **Validation performed:** tests or documentation checks run.
- **Skipped validation:** intentionally skipped checks and why.

## Agent skills

### Issue tracker

Specs and tickets use the local Markdown tracker under `.scratch/<feature-slug>/`. See `docs/agents/issue-tracker.md`.

### Triage labels

The tracker uses Matt's canonical triage roles and local lifecycle states. See `docs/agents/triage-labels.md`.

### Domain docs

This is a single-context repo: use root `CONTEXT.md` and relevant accepted ADRs under `docs/adr/`. See `docs/agents/domain.md`.

# AGENTS.md

Repository guidance for Matt's engineering skills and coding agents.

## Start Gate

1. Inspect the working tree with `git status --short`.
2. Treat every pre-existing modified or untracked file as user-owned unless the task explicitly targets it. Do not overwrite, reformat, delete, or clean unrelated changes.
3. Read only the context needed for the task:
   - Fetch the referenced GitHub issue or PRD through `gh`.
   - Read `CONTEXT.md` and relevant accepted ADRs when domain language or architecture is involved.
   - Read `README.md`, `CMakeLists.txt`, and the related code and tests for implementation work.
4. Stop exploring once the behavior, authoritative seam, constraints, and validation path are clear.

## Guardrails

1. **Data is passive value state.** Public contracts use aggregate-friendly `struct`, `std::variant`, `std::expected`, and project `util::JsonValue` values.
2. **Capabilities cross physical seams.** Chat clients, stream transports, execution environments, session stores, and tools are exposed through interfaces or dependency-heavy implementations hidden behind narrow headers.
3. **Events are weak connections.** Agent/provider event sinks use `std::move_only_function`; do not regress to copyability requirements such as `std::function`.
4. **Generic and serialization machinery stays local.** Glaze DTOs, schema conversion, visitors, parsing helpers, and similar machinery stay in serialization or implementation layers.
5. **Security and containment remain explicit.** Shell, file, environment-variable, provider, and session changes must preserve workspace containment, secret redaction, output truncation, and the documented “not a sandbox” boundary.

Do not reintroduce the legacy synchronous tool surface, `util::Result`, Boost.JSON domain contracts, `src` as a public include surface, or compatibility-only empty flags.

## pi C++ Parity

- The [pi C++ parity map](https://github.com/lanshengzhi/cpp-coding-harness/issues/2) is the current planning authority for open parity decisions.
- The local pi source checkout is `../pi`; a `pi:` reference resolves from that root.
- Inspect the relevant current pi source or documentation before making parity decisions or changes. Matching supported pi semantics is the default; record intentional divergences in the map or an accepted ADR.
- Preserve this repository's C++ idioms and guardrails rather than mechanically translating TypeScript.
- Approved work leaves the map and follows `/to-spec` → `/to-tickets` → `/implement`.
- Prefer the clean pi-aligned end state over migrations, fallback reads, deprecation shims, or compatibility-only flags unless a current contract explicitly requires them.

## Validation

- Use the build and test commands in `README.md`.
- During implementation, run the smallest focused test that can fail; run the full test suite once at the end as required by `/implement`.
- Run architecture tests when public headers, include surfaces, dependency directions, provider/tool/session contracts, or CMake public/private boundaries change.
- For documentation-only changes, check headings, links, tracker references, and clear agent-facing English; no C++ build is required.
- Use fake-provider tests by default. Do not use live API keys or network validation unless the user explicitly requests it.
- `/implement` includes review and a task-scoped commit on the current branch. Do not push, merge, switch branches, create worktrees, or delete branches unless the user explicitly asks.

## Agent skills

### Issue tracker

Issues and specs live in GitHub Issues. See `docs/agents/issue-tracker.md`.

### Triage labels

The tracker uses Matt's canonical triage labels. See `docs/agents/triage-labels.md`.

### Domain docs

This is a single-context repo: use root `CONTEXT.md` and `docs/adr/`. See `docs/agents/domain.md`.

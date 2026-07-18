# 06 — Add CLI no-session mode

**What to build:** Add an explicit `--no-session` target that runs a normal CLI Agent Session entirely in memory. The option works consistently across frontends and makes the absence of a persisted file visible rather than representing it with an empty path.

**Blocked by:** 04 — Add SDK in-memory Agent Sessions; 05 — Make CLI default sessions user-level

**Category:** enhancement

**Status:** implemented

- [x] CLI parsing accepts `--no-session` as an explicit session target and rejects combinations with explicit create or resume intent before model work.
- [x] CLI normalization maps `--no-session` to the shared in-memory target rather than a temporary path, empty path, or post-run cleanup convention.
- [x] Text one-shot, interactive REPL, JSON mode, and RPC mode can all create and use an in-memory Agent Session.
- [x] In-memory CLI prompts preserve normal provider, tool, event, Live Session State, error, and shutdown behavior.
- [x] `/session` and any frontend session information identify the Agent Session as in-memory and do not print an ambiguous empty file path.
- [x] `--no-session` creates no Agent Config Directory sessions root, workspace-local session directory, or transcript file, including after prompts and failures.
- [x] Storage unavailability is not silently treated as `--no-session`; in-memory operation occurs only when explicitly selected.
- [x] CLI parse and smoke tests cover invalid combinations, every frontend's target propagation, session information, prompt success, and zero filesystem side effects using the fake provider.
- [x] CLI help and current-behavior documentation explain the default-persisted and explicit in-memory choices.

## Comments

- `CliParse` maps `--no-session` to the shared `InMemorySessionTarget` variant and rejects `--session`/`--resume` combinations with precise messages after parse and before preflight or model work; the CLI11 `--session`/`--resume` excludes pair is unchanged.
- The normalized target flows through `CliConfig` → `AsyncCliRuntimeConfig` → `AgentSessionCreationRequest` into the existing `SessionFactory` in-memory branch, so all four frontends share one assembly path with no temporary-path or cleanup conventions.
- `CommandContext` gained an optional `session_path`; `/session` renders `File: <path>` for persisted sessions and an explicit `File: In-memory` (pi's session-info wording) for in-memory runs. RPC `get_state` reports `sessionFile` for persisted sessions and omits the key for in-memory operation, matching pi's `RpcSessionState`.
- New coverage: 4 CLI parse cases, 1 built-in command case, 1 RPC transcript case (plus the updated persisted `get_state` expectations), and 9 `[cli][no-session]` fake-provider smoke cases pinning text/JSON/RPC/REPL propagation, `/session` output for both targets, tool execution and events, invalid combinations before model work, zero filesystem state after prompts and a startup failure, and successful in-memory operation when default storage is unusable (the blocked-storage default-persistence failure tests from 05 pin the opposite direction).
- CLI help footer, README (run examples, Session storage, slash-command table, RPC mode), and module routing describe the default-persisted vs explicit in-memory choice.
- Validation: focused `[cli]`, `[cli][parse]`, `[coding_agent][prompt]`, and `[coding-agent][runtime][rpc]` slices passed; the complete CTest suite (670 tests) passed without live provider or network access.
- Review: standards review found no hard violations (deliberate notes retained: string-typed `CommandContext::session_path` matches neighboring fields, and the two parse-time combination rejections keep precise per-flag messages instead of CLI11 `excludes`). Spec review verified every checkbox with no scope creep toward ticket 07; its one gap note (tool execution under `--no-session`) was closed with a dedicated fake-provider tool-flow smoke case.

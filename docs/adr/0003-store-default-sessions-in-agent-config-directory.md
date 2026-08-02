---
status: accepted
---

# Store default sessions in the agent config directory

Default persisted Agent Session histories are user-level agent state associated with a workspace, not project artifacts. CLI and SDK default session creation therefore use `<agent-config-dir>/sessions/<encoded-workspace>/`, rooted through `AgentConfigDir`, instead of writing `.cpp-harness/sessions/` inside the process working directory. This mirrors pi's session ownership and layout while keeping one C++ session-assembly policy.

> The concrete directory root referenced here is superseded by [ADR 0030](0030-share-pi-agent-config-directory-and-credential-store.md): the Agent Config Directory is pi's own `~/.pi/agent`. The session-storage ownership and workspace-keyed layout decisions below are unchanged.
>
> The session-directory override variable `CCH_CODING_AGENT_SESSION_DIR` is superseded by [ADR 0031](0031-align-settings-shared-file-cli-and-resume-configuration-with-pi.md): pi's `PI_CODING_AGENT_SESSION_DIR` applies, removed without fallback. The precedence chain and resolution rules below are unchanged.

## Considered options

- Keep project-local default storage: rejected because transcripts are sensitive user state, default persistence should not modify or require write access to a workspace, and the existing path incorrectly follows process cwd rather than an explicit workspace.
- Centralize sessions without workspace partitioning: rejected because workspace association is needed for predictable lookup, cleanup, and future session discovery.
- Add migrations, fallback reads, or compatibility shims for the old default: rejected because this is an experimental project and explicit paths already cover intentional access to files outside the new default root.

## Consequences

- The default directory key uses pi's readable path encoding over the C++ factory's single canonical physical workspace value, so symlink aliases share one directory. Automatic file names use one UTC timestamp and UUID shared with the session header and SDK result.
- CLI automatic-directory precedence is `--session-dir`, `CCH_CODING_AGENT_SESSION_DIR`, `settings.json` `sessionDir`, then the workspace-keyed agent-config default. Relative session directories resolve against the final workspace. CLI-specific directory overrides do not implicitly affect SDK creation; SDK defaults derive from the Agent Config Directory and may select an explicit target instead.
- SDK session creation uses one variant target for default persistence, explicit creation, resume, or in-memory operation. Session paths are optional values because in-memory sessions have no file. CLI exposes an explicit `--no-session` mode.
- Explicit create and resume paths may remain outside the default root. Old project-local files are neither scanned nor migrated, but an explicitly supplied valid path remains usable.
- On POSIX, default-created session directories are owner-only (`0700`) and every new session file remains owner-readable/writable (`0600`), including files in explicit custom directories. Existing custom directories are not chmodded. Other platforms provide the closest supported protection. An unavailable or insufficiently private default store fails with the target path and reason; it never falls back to the workspace or silently becomes in-memory.
- Path computation is side-effect free. Directory and file creation happen only when the session is published, through a narrow Session Store capability used uniformly by JSONL and in-memory sessions.
- C++ intentionally retains its existing incremental durability semantics: failed assembly leaves no session file, while a successfully assembled session creates its header and persists completed user history even if a later provider step fails. pi's delayed first flush is not adopted.
- Session browsing, lookup by ID, selection, and deletion remain separate parity decisions. This decision does not implement those interfaces.

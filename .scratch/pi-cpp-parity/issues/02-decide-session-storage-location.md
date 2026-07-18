# Decide where session files live by default

Type: grilling
Status: resolved
Blocked by: None

## Question

Should default session storage stay project-local (`.cpp-harness/sessions/` inside the workspace, as today), or move into the agent config directory keyed by workspace, mirroring pi's `~/.pi/agent/sessions/<encoded-workspace>/`? Project-local keeps transcripts next to the workspace for inspection and cleanup; pi's layout keeps user-level state in one root and avoids writing harness state into workspaces.

## Answer

Move default persisted sessions into `~/.cpp-harness/agent/sessions/<encoded-canonical-workspace>/` for both CLI and SDK creation; see [ADR 0003](../../../docs/adr/0003-store-default-sessions-in-agent-config-directory.md). Agent Session history is user-level state associated with a workspace, not a project artifact.

The implementation effort must preserve these resolved constraints:

- Mirror pi's readable workspace-key encoding and UTC-timestamp-plus-UUID file naming, but encode the C++ factory's single canonical physical workspace so symlink aliases share sessions.
- CLI automatic-directory precedence is `--session-dir`, `CCH_CODING_AGENT_SESSION_DIR`, user `settings.json` `sessionDir`, then the agent-config default. Relative values resolve against the final workspace. These CLI-specific overrides do not implicitly affect SDK defaults.
- SDK uses one variant target covering default persistence, explicit creation, resume, and in-memory operation. Public session paths become optional, and CLI gains `--no-session`.
- Explicit paths remain unrestricted by the default root. Do not scan, migrate, or fall back to the old project-local default.
- Default directories and files are private. Storage failures are explicit and never fall back to the workspace or in-memory operation.
- Keep path computation side-effect free and put physical persistence behind one narrow Session Store capability.
- Preserve current C++ incremental durability semantics rather than pi's delayed first flush.
- Leave session browsing, lookup by ID, selection, and deletion to a separate parity decision.
- Prefer the clean pi-aligned end state: no compatibility shims, deprecated fields, or duplicate policy seams.

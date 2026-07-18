# 03 — Migrate SDK persisted session targets

**What to build:** Replace the SDK's mutually exclusive create/resume optional fields with one passive session-target value for default persisted creation, explicit persisted creation, or explicit resume. SDK callers that omit an explicit target receive a workspace-keyed persisted Agent Session under the Agent Config Directory, while explicit paths retain precise control.

**Blocked by:** 02 — Establish automatic session paths and safe publication

**Category:** enhancement

**Status:** implemented

- [x] SDK creation accepts one aggregate-friendly variant target with default-persisted, explicit-new, and explicit-resume value alternatives.
- [x] Default construction selects default persisted creation; the old independent create/resume optional fields are removed without aliases or compatibility adapters.
- [x] SDK default creation resolves one canonical physical workspace and reuses it for execution, Session Metadata, resume validation, and automatic path encoding.
- [x] Symbolic-link aliases of one physical workspace produce the same default workspace directory where the platform supports the test setup.
- [x] SDK default creation stores the session beneath the workspace-keyed Agent Config Directory sessions root and returns matching UUID, timestamp, metadata, and file path facts.
- [x] Explicit new and resume targets remain valid outside the default root and are never rewritten by automatic path policy.
- [x] Public SDK results and Agent Session path access use an optional path contract; persisted creation and resume return the actual path.
- [x] SDK creation fails explicitly when default storage has no resolvable root, cannot be created privately, or cannot publish the session; it does not fall back.
- [x] Failed assembly before publication leaves no session file, while a successfully assembled persisted session creates its header and retains current incremental durability behavior.
- [x] Old project-local default directories are not scanned, migrated, or used as fallback locations. Explicitly supplied valid files remain resumable.
- [x] Public SDK documentation and examples describe the target variant, default persistence, optional path, and intentional removal of the old experimental fields.
- [x] SDK, session lifecycle, path-policy, public-header, architecture, and focused persistence tests pass with no live provider or network access.

## Comments

- Replaced the experimental SDK create/resume optionals with `SessionTarget`, made default construction persist beneath the canonical workspace key in the Agent Config Directory, and exposed optional public path results without changing explicit-path behavior.
- Added SDK coverage for canonical and symbolic-link workspaces, path/metadata identity, CLI-only override isolation, publication and pre-publication failures, explicit targets, and successful/failing legacy-location non-fallback behavior.
- Validation: focused SDK, session lifecycle, path-policy, harness session, and architecture slices passed; the complete CTest suite passed without live provider or network access.
- Review: standards review found no hard violations; spec review passed after adding successful legacy-session ignore plus explicit-resume coverage. The remaining repeated target dispatch note is a low-risk judgment call retained to keep preparation and publication phases explicit.

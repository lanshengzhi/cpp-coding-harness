# 03 — Migrate SDK persisted session targets

**What to build:** Replace the SDK's mutually exclusive create/resume optional fields with one passive session-target value for default persisted creation, explicit persisted creation, or explicit resume. SDK callers that omit an explicit target receive a workspace-keyed persisted Agent Session under the Agent Config Directory, while explicit paths retain precise control.

**Blocked by:** 02 — Establish automatic session paths and safe publication

**Category:** enhancement

**Status:** ready-for-agent

- [ ] SDK creation accepts one aggregate-friendly variant target with default-persisted, explicit-new, and explicit-resume value alternatives.
- [ ] Default construction selects default persisted creation; the old independent create/resume optional fields are removed without aliases or compatibility adapters.
- [ ] SDK default creation resolves one canonical physical workspace and reuses it for execution, Session Metadata, resume validation, and automatic path encoding.
- [ ] Symbolic-link aliases of one physical workspace produce the same default workspace directory where the platform supports the test setup.
- [ ] SDK default creation stores the session beneath the workspace-keyed Agent Config Directory sessions root and returns matching UUID, timestamp, metadata, and file path facts.
- [ ] Explicit new and resume targets remain valid outside the default root and are never rewritten by automatic path policy.
- [ ] Public SDK results and Agent Session path access use an optional path contract; persisted creation and resume return the actual path.
- [ ] SDK creation fails explicitly when default storage has no resolvable root, cannot be created privately, or cannot publish the session; it does not fall back.
- [ ] Failed assembly before publication leaves no session file, while a successfully assembled persisted session creates its header and retains current incremental durability behavior.
- [ ] Old project-local default directories are not scanned, migrated, or used as fallback locations. Explicitly supplied valid files remain resumable.
- [ ] Public SDK documentation and examples describe the target variant, default persistence, optional path, and intentional removal of the old experimental fields.
- [ ] SDK, session lifecycle, path-policy, public-header, architecture, and focused persistence tests pass with no live provider or network access.

# 02 — Establish automatic session paths and safe publication

**What to build:** Add the internal, side-effect-free policy needed to derive and safely publish an automatic persisted Agent Session under the Agent Config Directory. This ticket establishes one reusable workspace-key, identity, naming, permission, and failure contract without yet changing CLI or SDK default target behavior.

**Blocked by:** 01 — Decouple Runtime from JSONL persistence

**Category:** enhancement

**Status:** implemented

- [x] The Agent Config Directory path contract exposes a derived sessions root and returns an empty value when no user-level root can be resolved.
- [x] One pure session-path policy derives the pi-shaped readable key from an already resolved workspace, including separator and drive-separator handling and double-hyphen framing.
- [x] Workspace keys use the agreed readable pi encoding without a hash suffix or legacy alternative.
- [x] Automatic identity generation produces a UUID Session ID and one UTC ISO-8601 timestamp shared by Session Metadata and the automatic filename.
- [x] The automatic filename contains the file-safe timestamp and the same UUID stored in the session header.
- [x] Computing the sessions root, workspace directory, identity, or target path creates no directory or file.
- [x] Automatic directory and file creation occurs only during session publication and retains exclusive file creation and symlink protections.
- [x] On POSIX, harness-owned default session directories are created or tightened to `0700`, and newly created session files are `0600`.
- [x] Existing explicit custom directories are not chmodded, while new files created inside them remain private.
- [x] A missing user-level root, an unwritable target, or a default directory that cannot be made private returns an error containing the attempted target and underlying reason.
- [x] Automatic publication never falls back to the workspace, another root, or an in-memory store.
- [x] Tests cover POSIX and Windows-shaped encoding inputs, UUID/header/filename correlation, path purity, symbolic-link defenses, permissions where supported, and failure diagnostics.
- [x] CMake membership and module routing identify one authoritative path-policy seam if new implementation modules are introduced.

## Comments

- Implemented the private `SessionPathPolicy` seam, derived Agent Config Directory sessions root, UUID/timestamp correlation, and publication-only default directory ownership rules without changing CLI or SDK target defaults.
- Hardened JSONL publication and later file access with descriptor-relative POSIX traversal, atomic exclusive creation, and Windows reparse-point/`CREATE_NEW` handling.
- Validation: focused Agent Config Directory, path-policy, Session Lifecycle, JSONL session, SDK assembly, and architecture slices passed; the complete CTest suite passed.
- Review: standards and ticket/spec reviews passed after resolving symlink-race, Windows exclusivity/compile, and relative-target findings.

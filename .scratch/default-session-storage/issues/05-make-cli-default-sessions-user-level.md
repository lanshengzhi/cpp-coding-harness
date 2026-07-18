# 05 — Make CLI default sessions user-level

**What to build:** Change CLI creation with no explicit session target to publish a persisted Agent Session in the workspace-keyed Agent Config Directory. The final workspace, not the process working directory, determines the default location. Existing explicit create and resume commands remain available.

**Blocked by:** 03 — Migrate SDK persisted session targets

**Category:** enhancement

**Status:** implemented

- [x] CLI normalization represents an omitted session target as default persisted creation instead of inventing a project-local file path in preflight.
- [x] SessionFactory consumes the normalized default target through the same private assembly policy used by SDK creation.
- [x] Launching from one directory with a different explicit workspace stores the automatic session under the explicit workspace's encoded key.
- [x] Symbolic-link aliases reuse the canonical physical workspace identity established by the shared policy.
- [x] Text, JSON, RPC, one-shot, and REPL startup all propagate the same default persisted target rather than choosing paths independently.
- [x] Explicit `--session` creation and `--resume` append behavior remains available and is not rewritten into the default root.
- [x] Automatic CLI creation does not scan, migrate, or fall back to the old project-local sessions directory.
- [x] A valid old project-local file remains usable only when passed explicitly through the general resume contract.
- [x] Missing or unsafe default storage fails before model work with an actionable path-and-reason error and no fallback transcript.
- [x] Failed CLI assembly leaves no visible session file; successful creation retains the current header publication and incremental message durability semantics.
- [x] CLI smoke coverage uses the fake provider and isolated Agent Config Directory state to verify actual path, metadata, UUID filename, workspace selection, explicit targets, and no legacy fallback.
- [x] CLI help, run examples, current-behavior documentation, and module routing describe automatic user-level persistence without claiming session browsing or ID lookup support.

## Comments

- Replaced the CLI's empty-path sentinel pair (`session_path`/`resume_path`) with the shared passive `SessionTarget` variant across `CliConfig`, `AsyncCliRuntimeConfig`, and `AgentSessionCreationRequest`; an omitted target default-constructs to default persisted creation.
- Removed the preflight `.cpp-harness/sessions/<timestamp>-<random>.jsonl` invention (`default_session_path`, `timestamp_for_path`, `random_suffix`); `normalize_cli` now maps the default target to the same `AutomaticNewSessionTarget` private assembly policy as the SDK via a shared `resolve_canonical_workspace` (renamed from `resolve_sdk_workspace`).
- CLI help footer and `--session` description document automatic user-level persistence; README run examples, the new Session storage section, the Agent config directory section, and module routing describe the default layout without claiming browsing or ID lookup.
- New `[cli][default-session]` smoke coverage (12 cases, fake provider + isolated `CCH_CODING_AGENT_DIR`): actual path/metadata/UUID filename correlation, explicit-workspace selection from a foreign launch directory, symlink alias sharing (POSIX), text/JSON/RPC/REPL target propagation, explicit targets outside the default root, no legacy fallback plus explicit-resume of the old file, unsafe and unresolvable default storage failures with attempted path and reason, help text, and failed-assembly cleanliness. New `[cli][parse][session-target]` cases pin the normalized representation.
- Validation: focused `[cli]`, `[coding-agent][runtime]`, `[sdk]`, `[coding_agent][session-path-policy]`, `[harness][session]`, `[sdk][in-memory]`, and architecture slices passed; the complete CTest suite (655 tests) passed without live provider or network access.
- Review: standards review found no hard violations (single-seam consolidation, passive target variant, ADR-0003 fidelity noted); spec review verified every checkbox with no scope creep toward tickets 06/07. Judgement-call notes retained: exhaustive variant cascades in `normalize_cli`/`normalize_sdk` mirror each other by design, the CLI `InMemorySessionTarget` branch stays unreachable until ticket 06, and the smoke-test key encoder is an intentional black-box oracle documented as such.

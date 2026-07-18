# 05 — Make CLI default sessions user-level

**What to build:** Change CLI creation with no explicit session target to publish a persisted Agent Session in the workspace-keyed Agent Config Directory. The final workspace, not the process working directory, determines the default location. Existing explicit create and resume commands remain available.

**Blocked by:** 03 — Migrate SDK persisted session targets

**Category:** enhancement

**Status:** ready-for-agent

- [ ] CLI normalization represents an omitted session target as default persisted creation instead of inventing a project-local file path in preflight.
- [ ] SessionFactory consumes the normalized default target through the same private assembly policy used by SDK creation.
- [ ] Launching from one directory with a different explicit workspace stores the automatic session under the explicit workspace's encoded key.
- [ ] Symbolic-link aliases reuse the canonical physical workspace identity established by the shared policy.
- [ ] Text, JSON, RPC, one-shot, and REPL startup all propagate the same default persisted target rather than choosing paths independently.
- [ ] Explicit `--session` creation and `--resume` append behavior remains available and is not rewritten into the default root.
- [ ] Automatic CLI creation does not scan, migrate, or fall back to the old project-local sessions directory.
- [ ] A valid old project-local file remains usable only when passed explicitly through the general resume contract.
- [ ] Missing or unsafe default storage fails before model work with an actionable path-and-reason error and no fallback transcript.
- [ ] Failed CLI assembly leaves no visible session file; successful creation retains the current header publication and incremental message durability semantics.
- [ ] CLI smoke coverage uses the fake provider and isolated Agent Config Directory state to verify actual path, metadata, UUID filename, workspace selection, explicit targets, and no legacy fallback.
- [ ] CLI help, run examples, current-behavior documentation, and module routing describe automatic user-level persistence without claiming session browsing or ID lookup support.

# 03 — Enforce external trust-store authority for SDK sessions

**What to build:** Session creation must use user-controlled trust authority rather than allowing a workspace to authorize itself. SDK callers may select an alternate trust store only when its resolved location is demonstrably outside the evaluated workspace.

**Blocked by:** 01 — Unify session creation behind a private assembly plan.

**Status:** ready-for-agent

- [ ] CLI and SDK session creation default to the user-level trust store.
- [ ] The SDK accepts an absolute trust-store path beneath a canonical external parent, including a not-yet-created trust file.
- [ ] Relative paths, the workspace itself, workspace descendants, canonicalization errors, and indeterminate containment fail closed.
- [ ] Existing files, symlinked files, and symlinked parent directories cannot bypass workspace containment checks.
- [ ] Workspace-local trust state cannot authorize discovered project resources.
- [ ] The public SDK contract documents the additive trust-store option and its containment rules.

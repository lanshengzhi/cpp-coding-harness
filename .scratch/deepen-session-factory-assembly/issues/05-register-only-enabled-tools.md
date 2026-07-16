# 05 — Expose only enabled tools to providers

Category: enhancement
**What to build:** CLI and SDK sessions must expose a tool registry derived from the final enabled capability set, so providers cannot see or request disabled built-ins while validated custom tools continue to work.

**Blocked by:** 01 — Unify session creation behind a private assembly plan.

**Status:** implemented

- [x] Disabled built-in tools, including disabled CLI bash, are absent from provider-visible tool schemas.
- [x] Enabled built-ins and valid custom tools are assembled through one shared registry policy.
- [x] Duplicate or otherwise invalid custom tool names fail session creation consistently across entry points.
- [x] Execution-environment permission checks remain in place as defense in depth.
- [x] CLI and SDK tests cover both enabled and disabled tool visibility.

## Comments

Implemented and verified as part of the completed SessionFactory assembly slice. The focused and full-suite command ledger is recorded in the [validation ledger](../../pi-agent-session-event-prompt-parity/issues/16-validate-and-close-plans.md).

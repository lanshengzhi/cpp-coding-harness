# 05 — Expose only enabled tools to providers

**What to build:** CLI and SDK sessions must expose a tool registry derived from the final enabled capability set, so providers cannot see or request disabled built-ins while validated custom tools continue to work.

**Blocked by:** 01 — Unify session creation behind a private assembly plan.

**Status:** ready-for-agent

- [ ] Disabled built-in tools, including disabled CLI bash, are absent from provider-visible tool schemas.
- [ ] Enabled built-ins and valid custom tools are assembled through one shared registry policy.
- [ ] Duplicate or otherwise invalid custom tool names fail session creation consistently across entry points.
- [ ] Execution-environment permission checks remain in place as defense in depth.
- [ ] CLI and SDK tests cover both enabled and disabled tool visibility.

# 07 — Add CLI session-directory overrides

**What to build:** Let CLI users redirect automatically created persisted sessions through a pi-shaped flag, environment variable, or User Settings preference with deterministic precedence. Relative values follow the final workspace, while explicit file targets and SDK defaults remain isolated from this CLI policy.

**Blocked by:** 05 — Make CLI default sessions user-level

**Category:** enhancement

**Status:** ready-for-agent

- [ ] CLI parsing accepts `--session-dir` as the highest-priority automatic-directory override and advertises it in help.
- [ ] `CCH_CODING_AGENT_SESSION_DIR` overrides User Settings and the Agent Config Directory default when no explicit flag is present.
- [ ] User Settings accepts the optional pi-vocabulary `sessionDir` field without regressing provider, trust, resource, unknown-key, or malformed-input behavior.
- [ ] Automatic-directory precedence is exactly explicit flag, dedicated environment variable, User Settings, then the workspace-keyed Agent Config Directory default.
- [ ] Relative override values resolve against the final canonical workspace rather than process cwd; absolute values remain absolute and leading-home values expand consistently.
- [ ] Explicit create and resume file targets remain more specific and are not rewritten by automatic-directory policy.
- [ ] CLI-specific directory flag, environment, and User Settings values do not affect SDK default persisted creation.
- [ ] Directory resolution remains side-effect free until publication and uses the same private-permission and explicit-failure behavior as the default root.
- [ ] CLI parse, settings-loader, SDK-isolation, and fake-provider smoke tests cover every precedence edge, relative and absolute values, home expansion, explicit targets, malformed settings, and unavailable targets.
- [ ] User documentation, settings documentation, CLI help, and module routing describe the override vocabulary and precedence without adding project-local settings support.

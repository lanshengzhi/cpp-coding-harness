# 07 — Add CLI session-directory overrides

**What to build:** Let CLI users redirect automatically created persisted sessions through a pi-shaped flag, environment variable, or User Settings preference with deterministic precedence. Relative values follow the final workspace, while explicit file targets and SDK defaults remain isolated from this CLI policy.

**Blocked by:** 05 — Make CLI default sessions user-level

**Category:** enhancement

**Status:** implemented

- [x] CLI parsing accepts `--session-dir` as the highest-priority automatic-directory override and advertises it in help.
- [x] `CCH_CODING_AGENT_SESSION_DIR` overrides User Settings and the Agent Config Directory default when no explicit flag is present.
- [x] User Settings accepts the optional pi-vocabulary `sessionDir` field without regressing provider, trust, resource, unknown-key, or malformed-input behavior.
- [x] Automatic-directory precedence is exactly explicit flag, dedicated environment variable, User Settings, then the workspace-keyed Agent Config Directory default.
- [x] Relative override values resolve against the final canonical workspace rather than process cwd; absolute values remain absolute and leading-home values expand consistently.
- [x] Explicit create and resume file targets remain more specific and are not rewritten by automatic-directory policy.
- [x] CLI-specific directory flag, environment, and User Settings values do not affect SDK default persisted creation.
- [x] Directory resolution remains side-effect free until publication and uses the same private-permission and explicit-failure behavior as the default root.
- [x] CLI parse, settings-loader, SDK-isolation, and fake-provider smoke tests cover every precedence edge, relative and absolute values, home expansion, explicit targets, malformed settings, and unavailable targets.
- [x] User documentation, settings documentation, CLI help, and module routing describe the override vocabulary and precedence without adding project-local settings support.

## Comments

- `UserSettings` gains the pi-vocabulary `sessionDir` string (parsed like the neighboring provider scalars: a non-string value is ignored, malformed JSON still hard-errors). `CliConfig`/`AsyncCliRuntimeConfig`/`AgentSessionCreationRequest` carry the raw `--session-dir` value into `SessionFactory`'s CLI normalization.
- Precedence lives in one private seam, `resolve_cli_session_dir_override` inside `normalize_cli`: first non-empty value of flag, `CCH_CODING_AGENT_SESSION_DIR`, settings `sessionDir` wins (pi's falsy-skip semantics); SDK normalization never consults any of them. The pure resolution `session_paths::resolve_session_dir_value` expands a leading `~`/`~/` against the new public `home_directory()` accessor (pi: `normalizePath`; `~other` stays literal), keeps absolute values, and resolves relative values against the final canonical workspace — the spec-mandated intentional divergence from pi's process-cwd behavior. Home expansion with no resolvable home fails explicitly before model work.
- A directory override replaces the whole automatic directory (pi: `SessionManager.create`): `make_custom_automatic_session_target` composes the file directly inside the override without a workspace-key component, marked by `AutomaticSessionTarget::custom_directory`. Publication stays side-effect free until the publish stage: `prepare_session_directory` now parameterizes final-dir privacy — harness-owned default roots are still created/tightened `0700`, while a custom override is created `0700` only when missing and an existing one's mode is never changed (ADR-0003); files remain `0600` and symlink/parent-traversal defenses apply to both shapes. Unavailable targets fail with attempted path and reason, no fallback.
- New coverage: 3 settings-loader cases, 4 CLI parse cases, 1 pure resolution case plus 6 custom composition/publication cases (side-effect freedom, `0700`/`0600`, preserved existing mode, symlink rejection, target-and-reason failure, relative rejection), and 9 `[cli][session-dir]` fake-provider smoke cases pinning flag>env>settings>default, relative-against-final-workspace from foreign launch dirs, `~` expansion, explicit create/resume isolation, `--no-session` isolation, non-string and malformed settings, and unavailable-target failure. The pre-existing SDK isolation case now guards genuinely parsed inputs.
- Docs: README run examples, Session storage override vocabulary/precedence, and settings paragraph; CLI help footer and `--session-dir` description; module-routing rows for agent config dir/settings, session lifecycle, and CLI wiring.
- Validation: focused `[settings]`, `[cli][parse]`, `[coding_agent][session-path-policy]`, `[cli]`, `[sdk]`, `[harness][session]`, and `[architecture]` slices passed; the complete suite (693 tests) passed without live provider or network access.
- Review: standards axis found no hard violations (single precedence seam, passive aggregates, ADR-0003 permission fidelity); fixed its include-style, duplicated home-check, and stale validation-message notes. Spec axis verified every checkbox with no scope creep; it confirmed the relative-to-workspace resolution is the spec-mandated intentional pi divergence. Judgement-call notes retained: the two automatic-target composition functions keep separate named policies rather than a shared builder, and empty override values fall through the precedence chain matching pi's falsy semantics (pinned in code comments).

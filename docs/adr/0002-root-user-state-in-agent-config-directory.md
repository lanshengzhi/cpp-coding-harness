---
status: accepted
---

# Root user-level state in a pi-mirrored agent config directory

User-level state files (auth entries, user settings, persisted project trust decisions) were scattered at the root of `~/.cpp-harness/` with their path literals duplicated across three loaders, and drift had already occurred (one file was moved without the others). pi keeps all user-level state under a single agent config directory (`~/.pi/agent/`) resolved through one `getAgentDir()` seam with a `PI_CODING_AGENT_DIR` override. This project adopts the same layout and vocabulary: one agent config directory at `~/.cpp-harness/agent/`, one public path module as the single source of truth, and pi's `settings.json` file vocabulary for user settings.

## Considered options

- Keep a flat `~/.cpp-harness/` root with per-file path literals at each call site: rejected because the drift that motivated this change is structural — nothing states the layout once, so any loader can be "tidied" independently of the others.
- Mirror pi's agent config directory but keep the local `config.json` file name: rejected because "config" then names both the directory concept and the file, while pi avoids exactly this collision by pairing its "agent config directory" with `settings.json`.
- Migrate or fall back to previously used locations: rejected because this is an experimental harness and the repository guardrails exclude compatibility-only machinery; the switch is hard and documented.
- Also relocate default session storage into the agent config directory now: deferred because session location is a separate product decision, tracked as decision ticket `02` in the pi parity map.

## Consequences

- `include/cch/coding_agent/AgentConfigDir.hpp` is the single source of user-level paths: `agent_config_dir()` resolves `CCH_CODING_AGENT_DIR` first, then `$HOME/.cpp-harness/agent` (`%USERPROFILE%` on Windows); `auth_file_path()`, `settings_file_path()`, and `trust_store_file_path()` derive from it and stay empty when no home is resolvable.
- The user settings file is `settings.json`; the domain vocabulary follows: `UserSettings` and `SettingsLoader` replace `ConfigData` and `ConfigLoader`, and "user settings" is the canonical term (see `CONTEXT.md`).
- Old locations are ignored without fallback reads; users move their files once.
- Future user-level features (global skills, prompt directories) land under the agent config directory through the same path module rather than inventing new roots.
- Default session storage was deferred to a separate product decision and has since been resolved by [ADR 0003](0003-store-default-sessions-in-agent-config-directory.md): default persisted sessions belong under the workspace-keyed agent config directory.

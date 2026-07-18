# 06 — Add CLI no-session mode

**What to build:** Add an explicit `--no-session` target that runs a normal CLI Agent Session entirely in memory. The option works consistently across frontends and makes the absence of a persisted file visible rather than representing it with an empty path.

**Blocked by:** 04 — Add SDK in-memory Agent Sessions; 05 — Make CLI default sessions user-level

**Category:** enhancement

**Status:** ready-for-agent

- [ ] CLI parsing accepts `--no-session` as an explicit session target and rejects combinations with explicit create or resume intent before model work.
- [ ] CLI normalization maps `--no-session` to the shared in-memory target rather than a temporary path, empty path, or post-run cleanup convention.
- [ ] Text one-shot, interactive REPL, JSON mode, and RPC mode can all create and use an in-memory Agent Session.
- [ ] In-memory CLI prompts preserve normal provider, tool, event, Live Session State, error, and shutdown behavior.
- [ ] `/session` and any frontend session information identify the Agent Session as in-memory and do not print an ambiguous empty file path.
- [ ] `--no-session` creates no Agent Config Directory sessions root, workspace-local session directory, or transcript file, including after prompts and failures.
- [ ] Storage unavailability is not silently treated as `--no-session`; in-memory operation occurs only when explicitly selected.
- [ ] CLI parse and smoke tests cover invalid combinations, every frontend's target propagation, session information, prompt success, and zero filesystem side effects using the fake provider.
- [ ] CLI help and current-behavior documentation explain the default-persisted and explicit in-memory choices.

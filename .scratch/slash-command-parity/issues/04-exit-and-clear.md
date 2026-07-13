# 04 — Implement `/exit` and text-frontend `/clear`

**What to build:** Add `/exit` as a true registry alias and implement `/clear` at the presentation seam so structured modes never receive terminal control bytes.

**Blocked by:** 02 — Add registry-owned command aliases.

**Status:** ready-for-agent

- [ ] Register `/exit` as an alias of `/quit`; keep the existing `shutdown_requested` result behavior.
- [ ] Make `/quit` and `/exit` terminate text, JSON, and RPC frontends after their terminal result is emitted, as specified by the PRD mode matrix; preserve and display the shutdown handler text where that mode carries human-readable messages.
- [ ] Add the existing `"shutdown"` result code to the public `PromptResult` documentation without changing the result shape.
- [ ] Register canonical `/clear` with metadata and a no-op command handler for non-text modes.
- [ ] The `/clear` handler returns `Usage: /clear` when arguments are present.
- [ ] Intercept exact `/clear` only in text frontend code for both REPL and one-shot text mode.
- [ ] Emit `\033[2J\033[H`, flush output, and do not call `AgentSession::prompt()` for the intercepted text command.
- [ ] Do not call `std::system`, a shell, or an execution environment.
- [ ] Ensure JSON and RPC consume `/clear` as `command_handled` with an empty message and no ANSI bytes.
- [ ] Add CLI smoke tests for text REPL, one-shot text, JSON, and RPC behavior, including shutdown display text and ANSI absence in structured output.
- [ ] In the RPC shutdown test, send a record after `/exit` and prove it is not processed after the shutdown terminal record.
- [ ] Run focused coding-agent, CLI, SDK, and architecture tests.

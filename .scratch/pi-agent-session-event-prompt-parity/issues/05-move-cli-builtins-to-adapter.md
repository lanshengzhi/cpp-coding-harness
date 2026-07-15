# 05 — Move text CLI built-ins to the CLI adapter

**What to build:** Keep the current text CLI commands working while making them frontend operations that are resolved before ordinary input reaches AgentSession.

**Blocked by:** None — can start immediately.

**Status:** implemented

- [x] Use PRD stories 34–35 and 41 and pi's separation between interactive built-ins and AgentSession prompting as the ownership authority.
- [x] Existing help, clear, exit, session, and other currently supported text commands preserve their observable text-mode behavior.
- [x] Recognized frontend commands do not enter AgentSession or prompt history; non-command input continues to AgentSession.
- [x] Shutdown and command presentation policy belong to the CLI adapter rather than the session runtime.
- [x] Focused command tests and the `[cli]` slice pass without live-provider access.

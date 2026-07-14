# 06 — Narrow AgentSession prompt interpretation

**What to build:** Give AgentSession one immutable skill-and-template interpretation pipeline while removing host-command registration and command-dispatch ownership from the session seam.

**Blocked by:** 05 — Move text CLI built-ins to the CLI adapter.

**Status:** ready-for-agent

- [ ] Use PRD stories 37–41 and the current pi skill/template expansion order as the contract authority.
- [ ] Skill invocation and prompt-template expansion still use the session's authorized immutable resource snapshot.
- [ ] Unmatched slash input remains an ordinary agent prompt.
- [ ] SDK host-command registration and CommandRegistry ownership are absent from AgentSession creation and prompt processing.
- [ ] No inert SessionExtension placeholder or compatibility command path is introduced.
- [ ] Focused prompt, skill/template, SDK, and architecture tests pass.

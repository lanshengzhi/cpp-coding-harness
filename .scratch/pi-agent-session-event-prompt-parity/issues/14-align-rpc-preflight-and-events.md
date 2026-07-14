# 14 — Align RPC preflight and event interleaving

**What to build:** Let RPC clients receive direct AgentSession events interleaved with response records, with prompt acceptance acknowledged only after AgentSession preflight succeeds.

**Blocked by:** 13 — Contract AgentSession to one prompt and subscription path.

**Status:** ready-for-agent

- [ ] Use PRD stories 31–33 and 36 and the current pi RPC prompt acknowledgement rules as the protocol authority.
- [ ] RPC emits direct AgentSession events and response records without a startup session header or runtime-terminal record.
- [ ] A valid prompt gets one success response after preflight acceptance and before later execution events; it never gets a second response for execution outcome.
- [ ] Closed, busy, empty, or otherwise preflight-rejected prompts produce RPC error responses without claiming acceptance.
- [ ] The currently supported command set, malformed-command recovery, shutdown, clean EOF, and response correlation remain adapter-owned and covered.
- [ ] Focused `[coding-agent][runtime][rpc]` and `[cli][rpc]` transcript tests pass.

# 12 — Emit direct pi-shaped JSON events

**What to build:** Give JSON-mode consumers a v3 session header followed by direct, semantically complete AgentSession events that remain safe to expose.

**Blocked by:** 04 — Align tool execution events and tool-result messages; 08 — Persist completed messages incrementally.

**Status:** ready-for-agent

- [ ] Use PRD stories 27–30 and the current pi JSON event-stream documentation as the wire-contract authority.
- [ ] The first record remains the v3 session header and every following record is a direct supported AgentSession event.
- [ ] `schemaVersion`, sequence counters, content-status substitution objects, wrapper envelopes, and runtime-terminal records are absent.
- [ ] Message and tool payloads retain their semantic structure after required secret redaction and bounded-output transformation.
- [ ] Serialization remains private and does not introduce Glaze or generic JSON machinery into public domain headers.
- [ ] Focused `[coding-agent][json-events]` and process-level JSON transcript tests pass.

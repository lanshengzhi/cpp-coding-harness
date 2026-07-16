# 12 — Emit direct pi-shaped JSON events

Category: enhancement
**What to build:** Give JSON-mode consumers a v3 session header followed by direct, semantically complete AgentSession events that remain safe to expose.

**Blocked by:** 04 — Align tool execution events and tool-result messages; 08 — Persist completed messages incrementally.

**Status:** implemented

- [x] Use spec stories 27–30 and the current pi JSON event-stream documentation as the wire-contract authority.
- [x] The first record remains the v3 session header and every following record is a direct supported AgentSession event.
- [x] `schemaVersion`, sequence counters, content-status substitution objects, wrapper envelopes, and runtime-terminal records are absent.
- [x] Message and tool payloads retain their semantic structure after required secret redaction and bounded-output transformation.
- [x] Serialization remains private and does not introduce Glaze or generic JSON machinery into public domain headers.
- [x] Focused `[coding-agent][json-events]` and process-level JSON transcript tests pass.

**Completion evidence:**
- JSON mode now subscribes once to AgentSession and writes the v3 header followed by direct supported agent, turn, message, and tool events; frontend-only command outcomes add no synthetic record.
- Private event serialization emits pi field names and complete message/tool structures, removes C++ tool-call recovery fields, and applies secret redaction plus UTF-8-safe per-value, collection, depth, node, string, and total-record bounds.
- The fake provider now emits assistant start before updates and carries pi-required assistant metadata for process-level lifecycle coverage.
- Focused JSON-event, process JSON, RPC regression, provider, session, and architecture slices pass; the full `ctest --preset vcpkg` suite passes.

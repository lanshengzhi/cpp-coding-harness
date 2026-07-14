# 15 — Synchronize domain, routing, and parity documentation

**What to build:** Give future maintainers an accurate map of AgentSession event ownership, prompt interpretation, live state, incremental persistence, and text/JSON/RPC adapter responsibilities.

**Blocked by:** 14 — Align RPC preflight and event interleaving.

**Status:** ready-for-agent

- [ ] Use PRD stories 46–47 and the completed implementation as the documentation authority.
- [ ] The domain glossary describes skill/template-only prompt interpretation, live state, subscriber ordering, and incremental message persistence.
- [ ] Routing and README guidance point agents to the correct AgentSession, CLI, JSON, RPC, session, and architecture validation slices.
- [ ] The parity roadmap and contract inventory no longer claim C++ schema v1, runtime-terminal records, committed-only state, per-prompt sinks, SDK commands, or queued-specific events.
- [ ] Unsupported extensions, queue APIs, retry, compaction, progress events, TUI, and other PRD exclusions remain explicitly deferred rather than represented by placeholders.
- [ ] Markdown headings, relative links, clear agent-oriented English, and no-information-loss checks pass; no C++ build is required for this docs-only ticket.

# 15 — Synchronize domain, routing, and parity documentation

**What to build:** Give future maintainers an accurate map of AgentSession event ownership, prompt interpretation, live state, incremental persistence, and text/JSON/RPC adapter responsibilities.

**Blocked by:** 14 — Align RPC preflight and event interleaving.

**Status:** implemented

- [x] Use PRD stories 46–47 and the completed implementation as the documentation authority.
- [x] The domain glossary describes skill/template-only prompt interpretation, live state, subscriber ordering, and incremental message persistence.
- [x] Routing and README guidance point agents to the correct AgentSession, CLI, JSON, RPC, session, and architecture validation slices.
- [x] The parity roadmap and contract inventory no longer claim C++ schema v1, runtime-terminal records, committed-only state, per-prompt sinks, SDK commands, or queued-specific events.
- [x] Unsupported extensions, queue APIs, retry, compaction, progress events, TUI, and other PRD exclusions remain explicitly deferred rather than represented by placeholders.
- [x] Markdown headings, relative links, clear agent-oriented English, and no-information-loss checks pass; no C++ build is required for this docs-only ticket.

## Comments

### Documentation updates

- `CONTEXT.md` now defines the live-state/subscriber/persistence ordering and the RPC preflight response contract without inventing a second prompt or event path.
- `README.md` now documents v3 session headers, incremental message persistence, live-versus-durable failure behavior, and focused AgentSession/JSON/RPC validation tags.
- `docs/agents/module-routing.md` routes live-state and incremental-persistence work through `AgentSessionRuntime`, SDK seam tests, session tests, and the existing architecture/RPC slices.
- The active parity roadmap and contract inventory now describe CLI-owned built-ins, skill/template-only AgentSession prompting, direct JSON/RPC events, preflight-aligned RPC responses, live state, incremental persistence, implemented session-tree contracts, and explicit deferrals.

### Validation

- Checked Markdown fence balance, heading structure, trailing whitespace, and relative links for every changed document.
- Searched the active roadmap and inventory for stale schema-v1, terminal-record, committed-only, per-prompt-sink, SDK-command, and queued-specific claims; remaining mentions state that those contracts are absent.
- Compared the updated ownership and ordering statements against PRD stories 46–47, implemented issues 01–14, and the focused test tags named in the docs.
- No C++ build or test run was performed because this ticket changes documentation only.

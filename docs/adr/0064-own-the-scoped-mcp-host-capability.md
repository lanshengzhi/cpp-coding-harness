---
status: accepted
---

# Own the scoped MCP host capability, Modern Era only

Pike gains a Model Context Protocol capability: the Pike Runtime acts as an **MCP Host** — a client holding In-Process Aggregation of configured Upstream MCP Servers — and nothing more. This decision records the role boundary, the protocol-era commitment, and the Supported/Deferred/Rejected capability split settled in the pike-mcp spec interviews. Terminology follows `CONTEXT.md` (MCP Host, Upstream MCP Server, In-Process Aggregation, Modern/Legacy Era, Lazy Tool Activation, Pending Elicitation, Upstream Connection Status).

## Decisions

- **Role boundary: client only.** Pike connects to Upstream MCP Servers and aggregates their tools for its own Agent Session. It exposes no MCP endpoint, runs no MCP daemon, and acts as no network proxy. A future server face is a separate ADR.
- **Modern Era only.** The wire surface is the 2026-07-28 revision: per-request `_meta` protocol fields, `server/discover`, Multi Round-Trip `input_required` results, and `subscriptions/listen`. There is no `initialize` handshake, no server-initiated request handling, and no 2024-11-05 HTTP+SSE transport. **Legacy Era is a Deferred Capability with a pre-cut seam**: the protocol stack selects an era adapter once per connection (modern probe via `server/discover`); the legacy adapter, if adopted later, is limited to `initialize` + `tools/list` + `tools/call` + ignoring notifications, and declares no client capabilities. Stdio transport ships together with that adapter as the ecosystem bridge; today's legacy-only stdio servers are unreachable until then, an accepted consequence recorded in the spec's Out of Scope.
- **Supported Capabilities (staged by the spec)**: tools (`tools/list`, `tools/call`); Pending Elicitation in form and URL modes via MRTR; cancellation and progress-notification receipt; server `instructions` surfacing; Lazy Tool Activation via transcript `toolsAdded`/`toolsRemoved`.
- **Deferred Capabilities**: resources, prompts, roots (must not reintroduce workspace containment semantics, ADR 0057), `subscriptions/listen` live updates, the tasks extension (not advertising it is interop-safe: servers must fall back to core blocking behavior), the Skills extension (sequenced after resources; executor-style `skills` tools already work as ordinary tools without it), Legacy Era + stdio.
- **Rejected**: sampling (deprecated upstream in 2026-07-28), logging (removed from the 2026-07-28 core), the 2024-11-05 HTTP+SSE transport, **MCP Apps** (interactive HTML UIs require a browser surface the Native TUI does not have, plus a remote-HTML security surface; revisitable only if a Web/GUI Projection ever exists), and a **client-side codemode interpreter** — pike adds no JS engine dependency; codemode benefits arrive for free through server-side codemode Upstreams (e.g. executor), and flat-server context economy is served by Lazy Tool Activation instead.
- **The advertised `clientCapabilities` set is the outward contract.** v1 advertises exactly `elicitation: {form, url}` — no `roots`, no `sampling`, no `extensions` — and every request carries that same set in `_meta`. The defensive matrix for non-conformant servers: an `inputRequests` entry of an undeclared type, an unrecognized `resultType`, or a `-32021 MissingRequiredClientCapability` error each degrade to a single failed tool call (ADR 0008 isolation), never a session failure.

## Considered options

- **Dual-era from day one** (codex, kimi-code, oh-my-pi all carry it): rejected for this spec. The legacy debt is not the handshake but the reverse-request plumbing and the old SSE transport; the pre-cut era seam keeps the door open without importing that debt. Every Supported feature would otherwise carry a ×2 test matrix from the start.
- **Client-side codemode** (opencode's `execute` + sandboxed JS interpreter): rejected. It is the heaviest single dependency surface in the surveyed implementations, duplicates what server-side codemode Upstreams already provide, and Lazy Tool Activation solves the same context-economy problem on pike's existing transcript mechanism (ADR 0060).
- **Eager injection with per-server allowlists only**: rejected as the sole context strategy — dozens of Upstreams would blow the model context; allowlists remain as configuration, but Lazy Tool Activation is the product answer.

## Consequences

- The wire layer keeps the protocol-version string and `io.modelcontextprotocol/*` `_meta` keys behind one private constants point. The conformance target is the released 2026-07-28 revision (frozen); tracking the post-release draft is a maintenance task, not a spec input.
- Package placement is recorded in ADR 0065 (fifth Capability Owner Package). Tool-name namespacing, approval/credential policy, and the TUI surface are spec-level Implementation Decisions settled in the same interview series; they require no further ADRs.
- Promoting any Deferred Capability requires its own issue, and an ADR where the seam is surprising.

## References

- Issue [#833](https://github.com/lanshengzhi/cpp-coding-harness/issues/833) (the MCP host spec this decision records).

- MCP specification, released revision 2026-07-28 (`schema/2026-07-28`), especially `basic/index` (per-request `_meta`, `resultType`), `server/discover`, `basic/patterns/mrtr`, `basic/versioning` (era model), and its Key Changes changelog.
- Reference survey: executor (dual-era server adapters, elicitation relay, grant model), codex (`codex-rmcp-client` capability checklist, deferred startup, hashed tool names), kimi-code (non-blocking initial load, needs-auth status), oh-my-pi (hand-rolled JSON-RPC precedent, edge-case test list), opencode (five-state connection status, codemode catalog instructions).
- ADR 0053 (product architecture contract authority), ADR 0057 (no workspace containment), ADR 0060 (transcript system messages), ADR 0061 (tool-execution rendering registry), ADR 0062 (text-limiting mechanics).

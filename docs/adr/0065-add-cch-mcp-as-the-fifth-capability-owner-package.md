---
status: accepted
---

# Add cch_mcp as the fifth Capability Owner Package

The MCP host capability (ADR 0064) is a fifth Capability Owner Package, `cch_mcp`, with source root `src/mcp/` and Owner Interface `<cch/mcp/...>`. It owns the MCP wire layer (JSON-RPC framing, era adapters, `io.modelcontextprotocol/*` request metadata), the Streamable HTTP transport, per-Upstream connection state machines (Upstream Connection Status), MRTR/Pending Elicitation values, defensive limits, and upstream credentials handling. It depends only on `cch_support`; its Owner Interface carries passive value contracts (`JsonValue`, `Expected`, `AsyncResult`) and never exposes Boost.Asio/Beast or exception types (ADR 0042, ADR 0046). The protocol-version string and reserved `_meta` keys live behind one private constants point inside the package (draft tracking, ADR 0064).

`cch_coding_agent` gains a legal Owner dependency on `cch_mcp`. `SessionFactory` remains the sole assembly point: it adapts discovered upstream tool descriptors into `cch::agent::Tool` values (Qualified Tool Name namespacing, approval-policy hook binding) and injects them into the `ToolRegistry`, mirroring how it binds harness file/shell to built-in tools today. The Parity Architecture Manifest records the new owner, its dependency edges, and the gate evidence; the headless-no-frontend invariant is unchanged — `frontend_tui` consumes upstream status and Pending Elicitation only through `cch_coding_agent` projections.

## Considered options

- **Private module inside `cch_coding_agent`**: rejected. The MCP stack (wire, transports, OAuth, MRTR) is a standalone capability, not session-management detail; burying it in the headless core blurs that owner's "Agent Session, Models Runtime" authority and hides a network-speaking module from the architecture gate's owner-level view.
- **Fold into `cch_agent_core`**: rejected. Agent core owns the agent loop and Tool behavior; giving it external-protocol reach would break its current shape (it holds no transports — wire adapters live in `cch_ai`).
- **Fold into `cch_ai`**: rejected. `cch_ai` owns model providers; MCP upstreams are not model providers and would drag provider-only policies (request authentication, wire specialization by `Model::provider`) into shapes they were never meant to cover.

## Consequences

- The owner table grows to five; `cmake/parity/manifest.json`, its validator evidence, and the architecture tests update in the same change as the package skeleton.
- Tool-name namespacing, approval policy, and the TUI surface for MCP are spec-level Implementation Decisions; they do not require manifest changes beyond this package edge.
- OAuth for remote Upstreams reuses the `cch_ai` OAuth *patterns* (browser flow, loopback callback server) but owns no shared code with `cch_ai`: the MCP OAuth client lives inside `cch_mcp`, keeping the two credential lifecycles independent.

## References

- Issue [#833](https://github.com/lanshengzhi/cpp-coding-harness/issues/833) (the MCP host spec); ADR 0064 (scoped MCP host capability), ADR 0039 (owner package graph and gate), ADR 0040 (AsyncResult/runtime lifecycle), ADR 0042 (no-exception core), ADR 0053 (product architecture contract), ADR 0054 (TLS-only client transports precedent).
- Reference survey recorded in ADR 0064.

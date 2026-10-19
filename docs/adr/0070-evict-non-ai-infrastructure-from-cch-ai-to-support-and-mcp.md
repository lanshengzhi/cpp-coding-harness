---
status: accepted
---

# Evict Non-AI Infrastructure from cch_ai to Support and MCP

Following [ADR 0068](0068-retire-cloud-provider-adapters-and-delegate-routing-to-magpie.md)'s
consolidation of model execution onto a single OpenAI Chat Completions SSE wire path via Magpie
and [ADR 0069](0069-consolidate-stream-lifecycle-and-terminal-error-state-machine.md)'s unification
of the stream lifecycle, this decision purifies the `cch_ai` Owner package by evicting all
domain-neutral transport, OAuth, and JSON/SSE parsing infrastructure from `src/ai/include/cch/ai/`
into `cch_support` and `cch_coding_agent`'s MCP subsystem.

## Context and motivation

After the cloud provider retirement in ADR 0068, `cch_ai` retained several infrastructure
mechanisms that were not specific to the AI domain:

1. **OAuth HTTP client and utilities in `cch_ai`**:
   `OAuthHttpClient.hpp`, `OAuthHttpClient.cpp`, and `OAuthShared.hpp` were temporarily retained in
   `src/ai/auth/` and exposed in `src/ai/include/cch/ai/` solely to serve the Model Context
   Protocol (`cch::coding_agent::mcp`) subsystem per ADR 0068 §Decision.2. `cch_ai` itself no longer
   had any cloud OAuth requirements.
2. **Neutral HTTP chunked streaming transport in `cch_ai`**:
   `StreamTransport.hpp`, `BoostBeastStreamTransport.hpp`, `BoostBeastStreamTransport.cpp`,
   `CancellationBridge.hpp`, `TransportExecutor.hpp`, and `TransportShared.hpp` were defined inside
   `cch_ai`. They provided generic HTTP/1.1 chunked streaming with TLS, body chunk callbacks, and
   cancellation hooks. None of these mechanisms possessed AI domain semantics.
3. **General-purpose JSON and SSE helpers in `cch_ai` public interface**:
   `JsonAccess.hpp` (type-safe JSON value and member extraction) and `SseParser.hpp` /
   `SseParser.cpp` (Server-Sent Events line buffering and chunk tokenization) were published under
   `<cch/ai/...>`. Both utilities are domain-neutral data-formatting primitives reused across
   multiple modules.

This distribution violated the Single Responsibility Principle and the deep module boundaries of
Pike's architecture:
- `cch_ai` is an Owner package whose domain is strictly AI inference, completion models, prompts,
  messages, tokens, and stream events. Exposing low-level HTTP transport interfaces and OAuth
  primitives bloated the owner boundary and created false dependencies.
- `cch_coding_agent`'s MCP subsystem was forced to depend on `<cch/ai/OAuthHttpClient.hpp>` and
  `<cch/ai/OAuthShared.hpp>` for its local authorization-code and PKCE token flows, creating an
  inverted architectural dependency where agent tooling reached into AI authentication.

## Decisions

1. **Supersede ADR 0068's temporary retention clause and move MCP OAuth to `cch_coding_agent`**:
   - Evict `OAuthHttpClient.hpp`, `OAuthHttpClient.cpp`, and `OAuthShared.hpp` out of `src/ai/auth/`
     and `src/ai/include/cch/ai/`.
   - Relocate them directly into `src/coding_agent/mcp/` under namespace `cch::coding_agent::mcp`.
   - Remove the `src/ai/auth/` directory entirely.

2. **Relocate generic Stream Transport infrastructure to `cch_support` as private implementation details**:
   - Move `StreamTransport.hpp`, `BoostBeastStreamTransport.hpp`, `BoostBeastStreamTransport.cpp`,
     `CancellationBridge.hpp`, `TransportExecutor.hpp`, and `TransportShared.hpp` to `src/support/`
     under namespace `cch::support`.
   - Adhere strictly to [ADR 0042](0042-adopt-a-strict-no-exception-core-with-private-async-bridges.md)
     and [ADR 0046](0046-move-pi-neutral-mechanics-from-cch-ai-to-cch-support.md): Boost.Beast and
     Boost.Asio transport headers remain private implementation details via quoted includes
     (`"support/StreamTransport.hpp"`, `"support/BoostBeastStreamTransport.hpp"`), and are never
     exposed in the public interface directory `<cch/support/...>`.
   - In `cch_ai`, `CompletionsPipeline` and `Models` consume `cch::support::StreamTransport` via
     these private support headers.

3. **Promote domain-neutral parsing utilities to `cch_support` public interface**:
   - Move `JsonAccess.hpp` to `src/support/include/cch/support/JsonAccess.hpp` under namespace
     `cch::support`.
   - Move `SseParser.hpp` to `src/support/include/cch/support/SseParser.hpp` and `SseParser.cpp` to
     `src/support/SseParser.cpp` under namespace `cch::support`.

4. **Purify the `cch_ai` public interface surface**:
   - The public interface root `src/ai/include/cch/ai/` now contains strictly AI domain types:
     `Content.hpp`, `Context.hpp`, `Error.hpp`, `Message.hpp`, `Models.hpp`, `RequestOptions.hpp`,
     `StreamEvent.hpp`, `Timestamps.hpp`, `Tool.hpp`, and `Usage.hpp`.
   - All non-AI transport, OAuth, and utility headers are completely evicted.

5. **Update build targets and tests**:
   - `cmake/targets/Support.cmake`: Add `SseParser.cpp` and `BoostBeastStreamTransport.cpp`.
   - `cmake/targets/Ai.cmake`: Remove `OAuthHttpClient.cpp`, `SseParser.cpp`, and `BoostBeastStreamTransport.cpp`.
   - `cmake/targets/CodingAgent.cmake`: Add `OAuthHttpClient.cpp` to `cch_coding_agent`.
   - Move transport and SSE tests to `tests/support/` and update `cmake/tests/SupportTests.cmake` and `cmake/tests/AiTests.cmake`.

## Consequences and guarantees

- **Pure AI Owner Interface**: `cch_ai`'s public interface is completely decoupled from HTTP
  transport engines, OAuth flows, and generic parsing primitives. Its public headers express only
  the core domain vocabulary of models and inference.
- **Deep and cohesive MCP module**: MCP OAuth client and token flows are fully co-located with the
  MCP protocol implementation in `src/coding_agent/mcp/`, eliminating cross-package reaching.
- **Strict boundary and no-exceptions guarantee**: Boost headers remain encapsulated in private
  support compilation units. The `-fno-exceptions` contract is preserved across all migrated targets.
- **Parity Architecture Gate**: The updated file structure and include boundaries pass all checks
  enforced by `cmake/parity/manifest.json` and `tests/architecture/PublicHeaderBoundaryTest.cpp`.

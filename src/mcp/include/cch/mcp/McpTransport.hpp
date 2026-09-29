#pragma once

#include <cch/support/AsyncResult.hpp>
#include <cch/support/Error.hpp>

#include <chrono>
#include <map>
#include <memory>
#include <stop_token>
#include <string>

namespace cch::mcp {

/// One Streamable HTTP exchange the MCP Host asks its transport to perform.
/// The client stack owns every byte above this boundary: `headers` already
/// carries the reserved `MCP-Protocol-Version` / `Mcp-Method` / `Mcp-Name`
/// set plus the `Mcp-Param-*` values mirrored from the tool's
/// `x-mcp-header` annotation, and `body` already holds the framed JSON-RPC
/// message.
struct McpRequest {
    std::string url{};
    /// The HTTP method. `POST` is the Streamable HTTP method the client stack
    /// uses for every JSON-RPC exchange and stays the default. `GET` exists
    /// for the OAuth discovery documents an authorization server publishes
    /// (RFC 8414, RFC 9728), which are fetched rather than posted; the
    /// production transport refuses any other method.
    std::string method{"POST"};
    std::map<std::string, std::string> headers{};
    std::string body{};
    /// Bounds connection setup, request dispatch, and response headers. The
    /// Streamable HTTP transport applies it; a scripted test transport that
    /// completes inline may ignore it.
    std::chrono::milliseconds timeout{std::chrono::seconds{30}};
    /// Active call cancellation. The transport answers a stopped request with
    /// a `Cancelled` error rather than by completing a result (ADR 0020,
    /// ADR 0040).
    std::stop_token stop_token{};
};

/// One transport answer, including non-success statuses so the client stack
/// can report an Upstream refusal as a protocol failure. `body` may carry
/// several framed JSON-RPC messages in order — a server notification may
/// arrive ahead of the response it belongs to — and the client stack reads
/// them in order.
struct McpResponse {
    int status_code{0};
    std::map<std::string, std::string> headers{};
    std::string body{};
};

/// The MCP Host's one transport seam (ADR 0065, ADR 0053 §Capability seams;
/// issue #836). Production supplies the TLS Streamable HTTP transport and
/// tests supply an in-memory scripted transport; every behavior above this
/// boundary — JSON-RPC framing, `_meta` and header injection, Multi
/// Round-Trip loops, error mapping, and cancellation — is driven through this
/// one class, so no second test seam exists.
///
/// Executor contract: the transport is driven by the caller's execution
/// domain and is not internally synchronized; run one connection's operations
/// on a single serialized executor and do not call `send` for the same
/// transport from two threads.
class McpTransport {
public:
    virtual ~McpTransport() = default;

    [[nodiscard]] virtual cch::support::AsyncResult<McpResponse> send(McpRequest request) = 0;
};

/// The MCP Host's production transport — Streamable HTTP over TLS — for the
/// owners that may not name an Asio type (ADR 0065). The returned transport
/// binds to the serialized execution domain that drives its first exchange
/// and reuses it afterwards, which is exactly the `McpTransport` executor
/// contract; no test seam is added, because tests inject a `McpTransport`
/// implementation directly (`tests/support/ScriptedMcpTransport.hpp`) and
/// this factory is the production answer only.
///
/// The transport is stateless between exchanges (ADR 0064: the 2026-07-28
/// revision has no `Mcp-Session-Id` and no SSE resumability), so one instance
/// serves every configured Upstream MCP Server in a session.
[[nodiscard]] std::shared_ptr<McpTransport> make_streamable_http_transport();

} // namespace cch::mcp

#pragma once

#include <cch/support/AsyncResult.hpp>
#include <cch/support/Error.hpp>

#include <chrono>
#include <map>
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

} // namespace cch::mcp

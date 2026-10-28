#pragma once

// Connection descriptor for one MCP server over the streamable HTTP transport
// (`StreamableHttpTransportOptions` in pi
// `packages/mcp/src/transports/streamable-http.ts`). Application configuration
// (`mcp.json` entries, OAuth blocks, exposure) stays in the application owner.

#include <cch/support/Error.hpp>

#include <chrono>
#include <map>
#include <string>

namespace cch::mcp {

struct HttpServerConfig {
    /// pi server name: the namespace of the server's tools and the identity
    /// used in diagnostics.
    std::string name;
    /// The server endpoint. pi v1.3.0 accepts `http://` and `https://` here
    /// (ADR 0075 supersedes ADR 0054's TLS-only rule for MCP); any other
    /// scheme is rejected at registration with pi's message. HTTPS
    /// certificate validation stays platform-enforced in the transport.
    std::string url;
    /// Extra request headers layered on every JSON-RPC POST (pi `headers`),
    /// for example an `Authorization` bearer token.
    std::map<std::string, std::string> headers;
    /// Per-request deadline. A request that gets no response by then fails
    /// with a timeout, and a cancellable request tells the server with
    /// `notifications/cancelled` reason `Request timed out` (pi `cancelPending`).
    std::chrono::milliseconds request_timeout{std::chrono::milliseconds{30000}};
};

/// Registration-time URL gate (pi `validateMcpServerConfig`): the URL must be
/// a well-formed `http://` or `https://` endpoint. Any other scheme is
/// rejected here with a Validation error before any socket work.
[[nodiscard]] support::ExpectedVoid validate_http_server_config(const HttpServerConfig& config);

} // namespace cch::mcp

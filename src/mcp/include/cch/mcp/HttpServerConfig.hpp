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
    /// The server endpoint. The current support boundary is TLS-only
    /// (ADR 0054): an `https://` URL is the only accepted form; plain HTTP and
    /// redirect handling are a later transport slice.
    std::string url;
    /// Extra request headers layered on every JSON-RPC POST (pi `headers`),
    /// for example an `Authorization` bearer token.
    std::map<std::string, std::string> headers;
    /// Per-request deadline. A request that gets no response by then fails
    /// with a timeout, and a cancellable request tells the server with
    /// `notifications/cancelled` reason `Request timed out` (pi `cancelPending`).
    std::chrono::milliseconds request_timeout{std::chrono::milliseconds{30000}};
};

/// Registration-time TLS gate (ADR 0054): the URL must be a well-formed
/// `https://` endpoint. A non-TLS URL is rejected here with a Validation
/// error; the transport never falls back to plaintext and never follows a
/// redirect to a non-TLS target.
[[nodiscard]] support::ExpectedVoid validate_http_server_config(const HttpServerConfig& config);

} // namespace cch::mcp

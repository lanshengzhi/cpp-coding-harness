#pragma once

#include "coding_agent/mcp/McpOAuthServerConfig.hpp"

#include <cch/support/Error.hpp>

#include <map>
#include <optional>
#include <string>

namespace cch::coding_agent::mcp {

/// Registration surface for one MCP server Pike reaches over the streamable
/// HTTP transport (`McpHttpServerConfig` in pi `core/mcp-servers.ts`, the
/// `url`/`headers` shape). The Session creation request holds the configured
/// list; full server management and persistence (pi `mcp.json`) is a later
/// slice, so this value is only the connection descriptor.
struct McpHttpServerConfig {
    /// pi server name: the namespace of the server's tools
    /// (`mcp__<name>__<tool>`) and the identity used in diagnostics.
    std::string name;
    /// The server endpoint. TLS-only (ADR 0054): an `https://` URL is the only
    /// accepted form; `http://` is rejected at registration.
    std::string url;
    /// Extra request headers layered on every JSON-RPC POST (pi `headers`),
    /// for example an `Authorization` bearer token.
    std::map<std::string, std::string> headers;
    /// OAuth sign-in configuration (spec #865, ticket #875). When set, every
    /// request resolves its access token at request time through the shared
    /// AuthStorage and attaches an `Authorization: Bearer` header; when absent,
    /// no credentials are attached. The credential itself lives in `auth.json`
    /// (`mcp_oauth_provider_id(name)`), never here.
    std::optional<McpOAuthServerConfig> oauth{std::nullopt};
};

/// Registration-time TLS gate (ADR 0054): the URL must be a well-formed
/// `https://` endpoint. A non-TLS URL is rejected here with a Validation
/// error; the transport never falls back to plaintext and never follows a
/// redirect to a non-TLS target.
[[nodiscard]] support::ExpectedVoid validate_mcp_http_server_config(const McpHttpServerConfig& config);

} // namespace cch::coding_agent::mcp

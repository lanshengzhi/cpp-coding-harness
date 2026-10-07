#pragma once

#include "coding_agent/mcp/McpOAuthConfig.hpp"
#include "coding_agent/mcp/McpOAuthServerConfig.hpp"
#include "coding_agent/mcp/McpServerConfigBase.hpp"

#include <cch/support/Error.hpp>

#include <chrono>
#include <map>
#include <optional>
#include <string>

namespace cch::coding_agent::mcp {

/// Registration surface for one MCP server Pike reaches over the streamable
/// HTTP transport (`McpHttpServerConfig` in pi `core/mcp-servers.ts`, the
/// `url`/`headers` shape). The Session creation request holds the configured
/// list; full server management and persistence (pi `mcp.json`) is a later
/// slice, so this value is only the connection descriptor.
struct McpHttpServerConfig : McpServerConfigBase {
    /// pi server name: the namespace of the server's tools
    /// (`mcp__<name>__<tool>`) and the identity used in diagnostics.
    std::string name;
    /// The server endpoint. TLS-only (ADR 0054): an `https://` URL is the only
    /// accepted form; `http://` is rejected at registration.
    std::string url;
    /// Extra request headers layered on every JSON-RPC POST (pi `headers`),
    /// for example an `Authorization` bearer token.
    std::map<std::string, std::string> headers;
    /// Per-request deadline (pi `timeout` seconds, default 60 in the config;
    /// pi's client default is 30 s). A request that gets no response by then
    /// fails with a timeout, and a cancellable request tells the server with
    /// `notifications/cancelled` reason `Request timed out` (pi `cancelPending`).
    std::chrono::milliseconds request_timeout{std::chrono::milliseconds{30000}};
    /// pi `oauth`: the entry's `oauth` block (spec #882, ticket #884). Its
    /// presence means the server uses OAuth; the authorization and token
    /// endpoints are discovered (RFC 9728), so they are not part of the entry.
    std::optional<McpOAuthConfig> oauth{std::nullopt};
    /// pi `auth.provider`: send the named `/login` provider's token instead of
    /// using OAuth. Only allowed in the global `mcp.json` (pi `readConfigFile`).
    std::optional<std::string> auth_provider{std::nullopt};
    /// The resolved OAuth endpoints (spec #865, ticket #875). Before OAuth
    /// discovery (spec #882) these are supplied by configuration; the
    /// credential itself lives in the shared credential store
    /// (`mcp_oauth_provider_id(name)`), never here.
    std::optional<McpOAuthServerConfig> resolved_oauth{std::nullopt};
};

/// Registration-time TLS gate (ADR 0054): the URL must be a well-formed
/// `https://` endpoint. A non-TLS URL is rejected here with a Validation
/// error; the transport never falls back to plaintext and never follows a
/// redirect to a non-TLS target.
[[nodiscard]] support::ExpectedVoid validate_mcp_http_server_config(const McpHttpServerConfig& config);

} // namespace cch::coding_agent::mcp

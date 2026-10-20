#pragma once

// Application registration surface for one streamable-HTTP MCP server: the
// generic connection descriptor plus the application's `mcp.json` entry
// fields and OAuth block. The transport itself is cch_mcp's.

#include "coding_agent/mcp/McpOAuthConfig.hpp"
#include "coding_agent/mcp/McpOAuthServerConfig.hpp"
#include "coding_agent/mcp/McpServerConfigBase.hpp"

#include <cch/mcp/HttpServerConfig.hpp>
#include <cch/support/Error.hpp>

#include <chrono>
#include <map>
#include <optional>
#include <string>

namespace cch::coding_agent::mcp {

struct McpHttpServerConfig : McpServerConfigBase, cch::mcp::HttpServerConfig {
    /// pi `oauth`: the entry's `oauth` block. Its presence means the server
    /// uses OAuth; the authorization and token endpoints are discovered.
    std::optional<McpOAuthConfig> oauth{std::nullopt};
    /// pi `auth.provider`: send the named provider's token instead of using
    /// OAuth. Only allowed in the global `mcp.json`.
    std::optional<std::string> auth_provider{std::nullopt};
    /// The resolved OAuth endpoints. The credential itself lives in the
    /// shared credential store (`mcp_oauth_provider_id(name)`), never here.
    std::optional<McpOAuthServerConfig> resolved_oauth{std::nullopt};
};

/// Registration-time TLS gate (ADR 0054), forwarding to the cch_mcp owner.
[[nodiscard]] support::ExpectedVoid validate_mcp_http_server_config(const McpHttpServerConfig& config);

} // namespace cch::coding_agent::mcp

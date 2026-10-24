#pragma once

// Application registration surface for one streamable-HTTP MCP server: the
// generic connection descriptor plus the application's `mcp.json` entry
// fields and OAuth block. The transport itself is cch_mcp's.

#include "coding_agent/mcp/McpServerConfigBase.hpp"

#include <cch/mcp/HttpServerConfig.hpp>
#include <cch/mcp/OAuthConfig.hpp>
#include <cch/mcp/OAuthServerConfig.hpp>
#include <cch/support/Error.hpp>

#include <chrono>
#include <map>
#include <optional>
#include <string>

namespace cch::coding_agent::mcp {

struct McpHttpServerConfig : McpServerConfigBase, cch::mcp::HttpServerConfig {
    /// pi `oauth`: the entry's `oauth` block. Its presence means the server
    /// uses OAuth; the authorization and token endpoints are discovered.
    std::optional<cch::mcp::OAuthConfig> oauth{std::nullopt};
    /// pi `auth.provider`: send the named provider's token instead of using
    /// OAuth. Only allowed in the global `mcp.json`.
    std::optional<std::string> auth_provider{std::nullopt};
    /// The resolved OAuth endpoints. The credential itself lives in the
    /// shared credential store (`mcp_oauth_provider_id(name)`), never here.
    std::optional<cch::mcp::OAuthServerConfig> resolved_oauth{std::nullopt};
};

} // namespace cch::coding_agent::mcp

#pragma once

// Bounded forwarding to the cch_mcp Owner Interface (extraction #1065).

#include <cch/mcp/OAuthServerConfig.hpp>

namespace cch::coding_agent::mcp {

using McpOAuthServerConfig = cch::mcp::OAuthServerConfig;

[[nodiscard]] inline support::ExpectedVoid validate_mcp_oauth_server_config(const McpOAuthServerConfig& config) {
    return cch::mcp::validate_oauth_server_config(config);
}

} // namespace cch::coding_agent::mcp

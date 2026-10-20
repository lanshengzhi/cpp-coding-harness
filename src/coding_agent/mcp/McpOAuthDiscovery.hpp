#pragma once

// Bounded forwarding to the cch_mcp Owner Interface (extraction #1065).

#include <cch/mcp/OAuthDiscovery.hpp>

namespace cch::coding_agent::mcp {

using McpAuthorizationServerMetadata = cch::mcp::AuthorizationServerMetadata;
using McpOAuthChallenge = cch::mcp::OAuthChallenge;
using McpOAuthDiscoveryOptions = cch::mcp::OAuthDiscoveryOptions;
using McpOAuthServerInfo = cch::mcp::OAuthServerInfo;
using McpProtectedResourceMetadata = cch::mcp::ProtectedResourceMetadata;
using cch::mcp::authorization_server_discovery_urls;
using cch::mcp::discover_authorization_server_metadata;
using cch::mcp::discover_oauth_server_info;
using cch::mcp::discover_protected_resource_metadata;
using cch::mcp::kDiscoveryProtocolVersion;
using cch::mcp::parse_authorization_server_metadata;
using cch::mcp::parse_protected_resource_metadata;
using cch::mcp::parse_www_authenticate;
using cch::mcp::select_resource;

inline constexpr std::string_view kMcpDiscoveryProtocolVersion = cch::mcp::kDiscoveryProtocolVersion;

} // namespace cch::coding_agent::mcp

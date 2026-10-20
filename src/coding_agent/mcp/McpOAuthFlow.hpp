#pragma once

// Bounded forwarding to the cch_mcp Owner Interface (extraction #1065).

#include <cch/mcp/OAuthFlow.hpp>

#include "coding_agent/mcp/McpAuthStore.hpp"
#include "coding_agent/mcp/McpOAuthConfig.hpp"
#include "coding_agent/mcp/McpOAuthDiscovery.hpp"

namespace cch::coding_agent::mcp {

using McpOAuthAuthorizationRequest = cch::mcp::OAuthAuthorizationRequest;
using McpOAuthClientInformation = cch::mcp::OAuthClientInformation;
using McpOAuthClientMetadata = cch::mcp::OAuthClientMetadata;
using McpOAuthClientMetadataDocument = cch::mcp::OAuthClientMetadataDocument;
using McpOAuthFlowOptions = cch::mcp::OAuthFlowOptions;
using McpOAuthFlowOutcome = cch::mcp::OAuthFlowOutcome;
using McpOAuthFlowResult = cch::mcp::OAuthFlowResult;
using McpOAuthTokenRequest = cch::mcp::OAuthTokenRequest;
using McpOAuthTokenResponse = cch::mcp::OAuthTokenResponse;
using cch::mcp::authorize_mcp;
using cch::mcp::client_metadata_document;
using cch::mcp::client_metadata_for;
using cch::mcp::derive_application_type;
using cch::mcp::exchange_authorization_code;
using cch::mcp::mcp_callback_id;
using cch::mcp::refresh_authorization;
using cch::mcp::register_client;
using cch::mcp::select_client_auth_method;
using cch::mcp::start_authorization;

} // namespace cch::coding_agent::mcp

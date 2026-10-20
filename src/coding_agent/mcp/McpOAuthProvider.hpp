#pragma once

// Bounded forwarding to the cch_mcp Owner Interface (extraction #1065). The
// store-coupled login/logout helpers remain application code because
// `mcp-auth.json` is application persistence.

#include "coding_agent/mcp/McpAuthStore.hpp"

#include <cch/mcp/OAuthProvider.hpp>

#include <cch/support/AsyncResult.hpp>

#include <memory>
#include <string>

namespace cch::coding_agent::mcp {

using McpOAuthProvider = cch::mcp::OAuthProvider;
using cch::mcp::mcp_oauth_credential_from_state;
using cch::mcp::mcp_oauth_state_from_credential;

/// Run one MCP server's OAuth sign-in through the existing `AuthInteraction`
/// surface and persist the tokens into `<agentDir>/mcp-auth.json`: the
/// `McpOAuthCredentialStore`-shaped state under `mcp__<server>|<url>`.
/// `provider` must outlive the returned operation: the continuation borrows it
/// and uses it when the operation is consumed asynchronously.
[[nodiscard]] support::AsyncResult<void> login_mcp_server(std::shared_ptr<McpAuthStore> store,
        std::string server_name,
        std::string server_url,
        McpOAuthProvider& provider,
        cch::mcp::AuthInteraction interaction);

/// Revoke one MCP server's stored credential: local removal from
/// `mcp-auth.json`, no server-side revocation. Missing records are not an
/// error.
[[nodiscard]] support::AsyncResult<void> logout_mcp_server(
        std::shared_ptr<McpAuthStore> store, std::string server_name, std::string server_url);

} // namespace cch::coding_agent::mcp

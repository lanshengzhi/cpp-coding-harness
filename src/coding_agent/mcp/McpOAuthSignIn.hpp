#pragma once

// The interactive and request-time sides of the MCP OAuth flow (spec #882,
// ticket #884). pi source at `7c10bd43` (v1.0.4):
// `packages/coding-agent/src/extensions/mcp/oauth.ts` (`callbackSettings`,
// `signInMcpServer`, `createMcpAuthProvider`). The flow itself
// (`McpOAuthFlow`) owns the wire protocol and starts no browser; this module
// gives it a loopback callback with a pasted-redirect-URL fallback, and a
// request-time `McpRequestAuthSource` that resolves an `oauth` block's tokens
// through discovery, dynamic client registration, and the refresh grant, so an
// `mcp.json` entry needs no pre-resolved endpoints.

#include "coding_agent/mcp/McpAuthStore.hpp"
#include "coding_agent/mcp/McpOAuthConfig.hpp"
#include "coding_agent/mcp/McpRequestAuthSource.hpp"

#include <cch/support/AsyncResult.hpp>

#include <boost/asio/awaitable.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>

namespace cch::ai::auth {
class OAuthHttpClient;
} // namespace cch::ai::auth

namespace cch::coding_agent::mcp {

/// pi `FALLBACK_REDIRECT_URL`: the redirect URI a request-time refresh sends
/// when the stored client registered none. Refreshing never redirects the
/// user, so any valid loopback URI the client accepts will do.
[[nodiscard]] std::string mcp_fallback_redirect_url();

/// pi `callbackSettings`: where the loopback callback listens and the redirect
/// URI it serves. `host` is the bind address (`localhost` is served on
/// 127.0.0.1), `redirect_host` the host name in the redirect URI, and
/// `fixed_redirect_url` is set only when the configuration names the port.
struct McpOAuthCallbackSettings {
    std::string host{"127.0.0.1"};
    std::string redirect_host{"127.0.0.1"};
    std::optional<std::uint16_t> port{std::nullopt};
    std::string path{"/callback"};
    std::optional<std::string> fixed_redirect_url{std::nullopt};
};

/// pi `callbackSettings(settings)` from an `mcp.json` `oauth` block.
[[nodiscard]] McpOAuthCallbackSettings mcp_oauth_callback_settings(const McpOAuthConfig& oauth);

/// The redirect URI for a sign-in: the fixed URI when the configuration names
/// one, else the callback's bound port.
[[nodiscard]] std::string mcp_oauth_bound_redirect_url(
        const McpOAuthCallbackSettings& settings, std::uint16_t bound_port);

/// pi `registeredRedirectUrls(state.clientInformation)[0] ?? FALLBACK`: the
/// redirect URI the request-time refresh sends for a stored registration.
[[nodiscard]] std::string mcp_oauth_registered_redirect_url(const std::optional<McpOAuthState>& state);

/// pi `signInPrompt` plus the CLI's `--timeout`: the authorization URL to show,
/// and the pasted-redirect-URL fallback for when the browser cannot reach the
/// loopback callback. The prompt resolves `std::nullopt` when the user cancels;
/// the caller aborts it (via the stop token) once the callback arrives or the
/// timeout elapses.
struct McpOAuthSignInPrompt {
    std::function<void(const std::string& authorization_url)> show_authorization_url;
    std::function<support::AsyncResult<std::optional<std::string>>(std::stop_token)> prompt_for_redirect_url;
};

/// pi `signInMcpServer`: sign in to one MCP server through the loopback
/// callback, racing the browser callback against the pasted redirect URL, and
/// persist the tokens into `mcp-auth.json`. A stored refresh token completes
/// the sign-in without a browser. A server-specific Client ID Metadata
/// Document (no RFC 9207 `iss`) also listens on `/callback/<callback id>`.
struct McpOAuthSignInRequest {
    std::shared_ptr<McpAuthStore> store;
    std::string server_name;
    std::string server_url;
    McpOAuthConfig oauth;
    std::chrono::milliseconds timeout{std::chrono::seconds{300}};
    /// The HTTPS client for discovery, registration, and the token grant.
    /// Defaults to the production `BoostBeastOAuthHttpClient`; tests inject a
    /// scripted one.
    std::shared_ptr<ai::auth::OAuthHttpClient> http{nullptr};
    McpOAuthSignInPrompt prompt;
};

/// Run one sign-in to completion and persist the result. A cancelled prompt, a
/// callback that never arrives before the timeout, or a failed exchange is an
/// explicit error; there is no unauthenticated result.
[[nodiscard]] support::AsyncResult<void> sign_in_mcp_server(McpOAuthSignInRequest request);

/// Request-time authentication through the OAuth flow (spec #882, ticket
/// #884): a stored, unexpired access token is sent as `Authorization: Bearer`
/// without any network call; otherwise the flow runs, which discovers the
/// authorization server, registers a client (caching it in `mcp-auth.json`),
/// and refreshes the stored tokens. A server that needs a browser sign-in fails
/// with the shared re-login error, so an `oauth` block with no pre-resolved
/// endpoints never reaches the server unauthenticated.
class McpOAuthFlowTokenResolver final : public McpRequestAuthSource {
public:
    McpOAuthFlowTokenResolver(std::shared_ptr<McpAuthStore> store,
            std::string server_name,
            std::string server_url,
            McpOAuthConfig oauth,
            std::shared_ptr<ai::auth::OAuthHttpClient> http = nullptr);

    McpOAuthFlowTokenResolver(McpOAuthFlowTokenResolver&&) noexcept;
    McpOAuthFlowTokenResolver& operator=(McpOAuthFlowTokenResolver&&) noexcept;
    ~McpOAuthFlowTokenResolver() override;
    McpOAuthFlowTokenResolver(const McpOAuthFlowTokenResolver&) = delete;
    McpOAuthFlowTokenResolver& operator=(const McpOAuthFlowTokenResolver&) = delete;

    [[nodiscard]] support::AsyncResult<std::map<std::string, std::string>> current_headers() override;

private:
    std::shared_ptr<McpAuthStore> store_;
    std::string server_name_;
    std::string server_url_;
    McpOAuthConfig oauth_;
    std::shared_ptr<ai::auth::OAuthHttpClient> http_;
};

} // namespace cch::coding_agent::mcp

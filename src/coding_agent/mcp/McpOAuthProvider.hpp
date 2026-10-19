#pragma once

#include "coding_agent/mcp/McpAuth.hpp"
#include "coding_agent/mcp/McpAuthStore.hpp"
#include "coding_agent/mcp/McpOAuthServerConfig.hpp"

#include <cch/support/AsyncResult.hpp>
#include <cch/support/Error.hpp>

#include <boost/asio/awaitable.hpp>

#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>

namespace cch::coding_agent::mcp {

class OAuthHttpClient;

/// The namespaced key used to identify one MCP server's OAuth credential.
/// It follows pi's MCP namespace (`mcp__<server>`, `-` replaced by `_`).
[[nodiscard]] std::string mcp_oauth_provider_id(std::string_view server_name);

/// MCP server OAuth sign-in and token lifecycle (spec #865, ticket #875):
/// authorization-code + PKCE S256 against the configured endpoints, a loopback
/// callback with a manual redirect-URL fallback for hosts that cannot reach it,
/// and refresh-token rotation. AuthInteraction belongs to MCP: notify shows
/// the authorization URL and prompt accepts the redirect URL. Login returns
/// MCP's credential; login_mcp_server persists it through McpAuthStore.
class McpOAuthProvider final {
public:
    /// A rejected `config` fails `login`/`refresh` through the returned error;
    /// the endpoints are also gated earlier at registration
    /// (`validate_mcp_oauth_server_config`). `http_client` defaults to the
    /// production HTTPS client; tests inject a scripted one.
    explicit McpOAuthProvider(McpOAuthServerConfig config, std::shared_ptr<OAuthHttpClient> http_client = nullptr);

    McpOAuthProvider(McpOAuthProvider&&) noexcept;
    McpOAuthProvider& operator=(McpOAuthProvider&&) noexcept;
    ~McpOAuthProvider();
    McpOAuthProvider(const McpOAuthProvider&) = delete;
    McpOAuthProvider& operator=(const McpOAuthProvider&) = delete;

    /// Run the interactive sign-in and return the credential. A missing prompt
    /// hook, a stopped interaction, a failed exchange, or a token response
    /// without an access token is an explicit error; there is no unauthenticated
    /// result.
    [[nodiscard]] boost::asio::awaitable<support::Expected<OAuthCredential>> login(AuthInteraction interaction);

    /// Exchange the refresh token for a rotated access token. An
    /// `invalid_grant` (a dead or already-rotated refresh token) is reported as
    /// an OAuth error the caller turns into an explicit re-login error; it is
    /// never retried and never yields an empty credential.
    [[nodiscard]] boost::asio::awaitable<support::Expected<OAuthCredential>> refresh(OAuthCredential credential);

private:
    [[nodiscard]] boost::asio::awaitable<support::Expected<OAuthCredential>> request_token(
            std::string body, std::stop_token stop_token);

    McpOAuthServerConfig config_;
    std::shared_ptr<OAuthHttpClient> http_client_;
};

/// The `McpOAuthState` of an `OAuthCredential` for `server_url`: the token
/// pair plus the expiry, in pi's `mcp-auth.json` shape.
[[nodiscard]] McpOAuthState mcp_oauth_state_from_credential(const OAuthCredential& credential, std::string server_url);

/// The `OAuthCredential` of a stored state, or `std::nullopt` when the
/// state carries no tokens. A state without `tokensExpireAt` maps to the
/// no-reported-expiry sentinel, matching the login path.
[[nodiscard]] std::optional<OAuthCredential> mcp_oauth_credential_from_state(const McpOAuthState& state);

/// Run one MCP server's OAuth sign-in through the existing `AuthInteraction`
/// surface and persist the tokens into `<agentDir>/mcp-auth.json` (spec #882,
/// ticket #884): the pi `McpOAuthCredentialStore`-shaped state under
/// `mcp__<server>|<url>`. The in-memory contract stays the existing
/// `OAuthCredential`; the store is the only write path. `server_name` and
/// `server_url` identify the state key.
[[nodiscard]] support::AsyncResult<void> login_mcp_server(std::shared_ptr<McpAuthStore> store,
        std::string server_name,
        std::string server_url,
        McpOAuthProvider& provider,
        AuthInteraction interaction);

/// Revoke one MCP server's stored credential: local removal from
/// `mcp-auth.json` (pi `McpOAuthCredentialStore.remove`), no server-side
/// revocation. Missing records are not an error.
[[nodiscard]] support::AsyncResult<void> logout_mcp_server(
        std::shared_ptr<McpAuthStore> store, std::string server_name, std::string server_url);

} // namespace cch::coding_agent::mcp

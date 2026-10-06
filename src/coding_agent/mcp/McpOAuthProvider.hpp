#pragma once

#include "coding_agent/mcp/McpOAuthServerConfig.hpp"

#include <cch/ai/Auth.hpp>
#include <cch/ai/CredentialStore.hpp>
#include <cch/support/AsyncResult.hpp>
#include <cch/support/Error.hpp>

#include <boost/asio/awaitable.hpp>

#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>

namespace cch::ai::auth {
class OAuthHttpClient;
} // namespace cch::ai::auth

namespace cch::coding_agent::mcp {

/// The shared `auth.json` key of one MCP server's OAuth credential (spec #865,
/// ticket #875). It follows pi's MCP namespace (`mcp__<server>`, `-` replaced
/// by `_`), so a server named `radius` stores one `ai::OAuthCredential` under
/// `mcp__radius` — a new provider id in the existing credential store, never a
/// second store.
[[nodiscard]] std::string mcp_oauth_provider_id(std::string_view server_name);

/// MCP server OAuth sign-in and token lifecycle (spec #865, ticket #875):
/// authorization-code + PKCE S256 against the configured endpoints, a loopback
/// callback with a manual redirect-URL fallback for hosts that cannot reach it,
/// and refresh-token rotation. The credential contract is the existing
/// `ai::OAuthCredential`; `login` reuses the existing `AuthInteraction`
/// presentation (`notify` shows the authorization URL, `prompt` accepts the
/// redirect URL) and the shared OAuth helpers, so the flow has the same shape as
/// the built-in provider flows and the user learns no second credential story.
///
/// The provider performs no persistence: login returns the credential and the
/// caller (see `login_mcp_server`) writes it through
/// `ai::CredentialStore::modify`, the only write path.
class McpOAuthProvider final {
public:
    /// A rejected `config` fails `login`/`refresh` through the returned error;
    /// the endpoints are also gated earlier at registration
    /// (`validate_mcp_oauth_server_config`). `http_client` defaults to the
    /// production HTTPS client; tests inject a scripted one.
    explicit McpOAuthProvider(
            McpOAuthServerConfig config, std::shared_ptr<ai::auth::OAuthHttpClient> http_client = nullptr);

    McpOAuthProvider(McpOAuthProvider&&) noexcept;
    McpOAuthProvider& operator=(McpOAuthProvider&&) noexcept;
    ~McpOAuthProvider();
    McpOAuthProvider(const McpOAuthProvider&) = delete;
    McpOAuthProvider& operator=(const McpOAuthProvider&) = delete;

    /// Run the interactive sign-in and return the credential. A missing prompt
    /// hook, a stopped interaction, a failed exchange, or a token response
    /// without an access token is an explicit error; there is no unauthenticated
    /// result.
    [[nodiscard]] boost::asio::awaitable<support::Expected<ai::OAuthCredential>> login(ai::AuthInteraction interaction);

    /// Exchange the refresh token for a rotated access token. An
    /// `invalid_grant` (a dead or already-rotated refresh token) is reported as
    /// an OAuth error the caller turns into an explicit re-login error; it is
    /// never retried and never yields an empty credential.
    [[nodiscard]] boost::asio::awaitable<support::Expected<ai::OAuthCredential>> refresh(
            ai::OAuthCredential credential);

    /// The request-time authentication value for a resolved credential: the
    /// `Authorization: Bearer <access>` header pi's MCP transport sends.
    [[nodiscard]] boost::asio::awaitable<support::Expected<ai::ModelAuth>> to_auth(
            const ai::OAuthCredential& credential) const;

private:
    [[nodiscard]] boost::asio::awaitable<support::Expected<ai::OAuthCredential>> request_token(
            std::string body, std::stop_token stop_token);

    McpOAuthServerConfig config_;
    std::shared_ptr<ai::auth::OAuthHttpClient> http_client_;
};

/// Run one MCP server's OAuth sign-in through the existing `AuthInteraction`
/// surface and persist the credential into the shared `auth.json`, exactly as
/// `ai::Models::login` does for a model provider: `CredentialStore::modify` is
/// the only write path and a store failure is the only failure wrapped as
/// `auth`. `provider_id` is `mcp_oauth_provider_id(server_name)`.
[[nodiscard]] support::AsyncResult<void> login_mcp_server(std::shared_ptr<ai::CredentialStore> credentials,
        std::string provider_id,
        McpOAuthProvider& provider,
        ai::AuthInteraction interaction);

/// Revoke one MCP server's stored credential (local removal through
/// `CredentialStore::remove`, matching the existing logout semantics: no
/// server-side revocation). Missing records are not an error.
[[nodiscard]] support::AsyncResult<void> logout_mcp_server(
        std::shared_ptr<ai::CredentialStore> credentials, std::string provider_id);

} // namespace cch::coding_agent::mcp

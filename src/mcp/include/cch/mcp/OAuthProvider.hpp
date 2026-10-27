#pragma once

#include <cch/mcp/Auth.hpp>
#include <cch/mcp/OAuthServerConfig.hpp>
#include <cch/mcp/OAuthState.hpp>

#include <cch/support/AsyncResult.hpp>
#include <cch/support/Error.hpp>

#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>

namespace cch::mcp {

class OAuthHttpClient;

/// MCP server OAuth sign-in and token lifecycle (spec #865, ticket #875):
/// authorization-code + PKCE S256 against the configured endpoints, a loopback
/// callback with a manual redirect-URL fallback for hosts that cannot reach it,
/// and refresh-token rotation. AuthInteraction belongs to MCP: notify shows
/// the authorization URL and prompt accepts the redirect URL. Login returns
/// MCP's credential; login_mcp_server persists it through OAuthStateStore.
class OAuthProvider final {
public:
    /// A rejected `config` fails `login`/`refresh` through the returned error;
    /// the endpoints are also gated earlier at registration
    /// (`validate_mcp_oauth_server_config`). `http_client` defaults to the
    /// production HTTPS client; tests inject a scripted one.
    explicit OAuthProvider(OAuthServerConfig config, std::shared_ptr<OAuthHttpClient> http_client = nullptr);

    OAuthProvider(OAuthProvider&&) noexcept;
    OAuthProvider& operator=(OAuthProvider&&) noexcept;
    ~OAuthProvider();
    OAuthProvider(const OAuthProvider&) = delete;
    OAuthProvider& operator=(const OAuthProvider&) = delete;

    /// Run the interactive sign-in and return the credential. A missing prompt
    /// hook, a stopped interaction, a failed exchange, or a token response
    /// without an access token is an explicit error; there is no unauthenticated
    /// result.
    [[nodiscard]] support::AsyncResult<OAuthCredential> login(AuthInteraction interaction);

    /// Exchange the refresh token for a rotated access token. An
    /// `invalid_grant` (a dead or already-rotated refresh token) is reported as
    /// an OAuth error the caller turns into an explicit re-login error; it is
    /// never retried and never yields an empty credential.
    [[nodiscard]] support::AsyncResult<OAuthCredential> refresh(
            OAuthCredential credential, std::stop_token stop_token = {});

private:
    OAuthServerConfig config_;
    std::shared_ptr<OAuthHttpClient> http_client_;
};

/// The `OAuthState` of an `OAuthCredential` for `server_url`: the token
/// pair plus the expiry, in pi's `mcp-auth.json` shape.
[[nodiscard]] OAuthState mcp_oauth_state_from_credential(const OAuthCredential& credential, std::string server_url);

/// The `OAuthCredential` of a stored state, or `std::nullopt` when the
/// state carries no tokens. A state without `tokensExpireAt` maps to the
/// no-reported-expiry sentinel, matching the login path.
[[nodiscard]] std::optional<OAuthCredential> mcp_oauth_credential_from_state(const OAuthState& state);

} // namespace cch::mcp

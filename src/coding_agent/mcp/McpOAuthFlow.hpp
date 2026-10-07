#pragma once

// The MCP OAuth authorization flow (spec #882, ticket #884): dynamic client
// registration with SEP-837 `application_type`, Client ID Metadata Documents,
// PKCE S256 authorization, the authorization-code and refresh grants, and pi's
// error semantics. pi source at `7c10bd43` (v1.0.4):
// `packages/mcp/src/oauth/flow.ts` (`authorizeMcp`, `registerClient`,
// `startAuthorization`, `exchangeAuthorizationCode`, `refreshAuthorization`,
// `selectClientAuthMethod`, `applicationType`) and
// `packages/coding-agent/src/extensions/mcp/oauth.ts` (`clientMetadataDocument`,
// `signInMcpServer`). The credential state is pi's `mcp-auth.json` state
// (`McpAuthStore`), the HTTPS transport is the injectable
// `ai::auth::OAuthHttpClient`, and the authorization code itself is supplied by
// the caller (the loopback callback or a pasted redirect URL), so this module
// starts no browser and owns no presentation.

#include "coding_agent/mcp/McpAuthStore.hpp"
#include "coding_agent/mcp/McpOAuthConfig.hpp"
#include "coding_agent/mcp/McpOAuthDiscovery.hpp"

#include <cch/support/Error.hpp>

#include <boost/asio/awaitable.hpp>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cch::ai::auth {
class OAuthHttpClient;
} // namespace cch::ai::auth

namespace cch::coding_agent::mcp {

/// pi `OAuthTokens`: one token-endpoint response. `scope` is recorded because a
/// response without one grants the requested scope (RFC 6749 §5.1).
struct McpOAuthTokenResponse {
    std::string access_token;
    std::string token_type;
    std::optional<double> expires_in;
    std::optional<std::string> scope;
    std::optional<std::string> refresh_token;
    std::optional<std::string> id_token;
};

/// pi `OAuthClientMetadataDocument`: an `https` URL used as `client_id`, plus
/// the redirect URI the document lists (which may carry a server-specific
/// path).
struct McpOAuthClientMetadataDocument {
    std::string url;
    std::string redirect_url;
};

/// pi `OAuthClientMetadata` with the defaults `McpOAuthProvider` applies: the
/// redirect URIs, the grant and response types, and the token-endpoint auth
/// method derived from the configured client secret.
struct McpOAuthClientMetadata {
    std::vector<std::string> redirect_uris;
    std::string token_endpoint_auth_method;
    std::vector<std::string> grant_types;
    std::vector<std::string> response_types;
    std::string client_name;
};

/// The client metadata for one sign-in: `redirect_url`, `oauth.client_name`
/// (pi's default `pi`), and `none` unless a client secret is configured.
[[nodiscard]] McpOAuthClientMetadata client_metadata_for(std::string_view redirect_url, const McpOAuthConfig& oauth);

/// pi `applicationType` (MCP SEP-837): `native` when any redirect URI is a
/// custom scheme or a loopback host, else `web`. Without it OpenID Connect
/// servers assume `web` and reject an `http` loopback redirect URI.
[[nodiscard]] std::string derive_application_type(const std::vector<std::string>& redirect_uris);

/// pi `selectClientAuthMethod`: the client's own hint when the server supports
/// it, else a method the server lists, else `client_secret_basic` /
/// `client_secret_post` / `none` by whether a secret exists.
[[nodiscard]] std::string select_client_auth_method(
        const McpOAuthClientInformation& client, const std::vector<std::string>& supported_methods);

/// pi's Client ID Metadata Document for `clientRegistration: "cimd"`. It
/// requires the server to advertise `client_id_metadata_document_supported` and
/// a `none` token-endpoint auth method; without RFC 9207 `iss` support the
/// document and the redirect URI are specific to this MCP server.
[[nodiscard]] support::Expected<McpOAuthClientMetadataDocument> client_metadata_document(std::string_view server_url,
        std::string_view redirect_url,
        const std::optional<McpAuthorizationServerMetadata>& metadata);

/// pi `callbackId`: the 12-character token that identifies `server_url` in the
/// server-specific Client ID Metadata Document's redirect path
/// (`/callback/<id>`). The sign-in listens on that path so a browser callback
/// for a server-specific document arrives.
[[nodiscard]] support::Expected<std::string> mcp_callback_id(std::string_view server_url);

/// pi `registerClient`: POST the client metadata — plus the derived
/// `application_type` and the requested `scope` — to
/// `registration_endpoint`, or `/register` on the authorization server when it
/// does not publish one. A non-2xx response is an explicit registration error
/// naming the status and body; no client is invented.
[[nodiscard]] boost::asio::awaitable<support::Expected<McpOAuthClientInformation>> register_client(
        std::shared_ptr<ai::auth::OAuthHttpClient> http,
        std::string authorization_server_url,
        std::optional<McpAuthorizationServerMetadata> metadata,
        McpOAuthClientMetadata client_metadata,
        std::optional<std::string> scope);

/// pi `startAuthorization` result: the URL to open and the PKCE verifier that
/// belongs to it.
struct McpOAuthAuthorizationRequest {
    std::string url;
    std::string code_verifier;
};

/// pi `startAuthorization`: PKCE S256 against `metadata.authorization_endpoint`
/// (or `/authorize` on the authorization server), with `response_type=code`,
/// the client id, the redirect URI, the optional state, scope and `resource`,
/// and `prompt=consent` when the scope asks for `offline_access`. A server that
/// does not support the authorization code grant or PKCE S256 is rejected
/// before the URL is built.
[[nodiscard]] support::Expected<McpOAuthAuthorizationRequest> start_authorization(
        std::string_view authorization_server_url,
        const std::optional<McpAuthorizationServerMetadata>& metadata,
        const McpOAuthClientInformation& client,
        std::string_view redirect_url,
        const std::optional<std::string>& scope,
        const std::optional<std::string>& state,
        const std::optional<std::string>& resource);

/// The inputs shared by both token grants.
struct McpOAuthTokenRequest {
    std::optional<McpAuthorizationServerMetadata> metadata;
    McpOAuthClientInformation client;
    std::optional<std::string> resource;
};

/// pi `exchangeAuthorizationCode`: the authorization-code grant with the PKCE
/// verifier and the exact redirect URI, authenticated by
/// `select_client_auth_method`. An OAuth error body is read before the status,
/// as servers report errors with any status.
[[nodiscard]] boost::asio::awaitable<support::Expected<McpOAuthTokenResponse>> exchange_authorization_code(
        std::shared_ptr<ai::auth::OAuthHttpClient> http,
        std::string authorization_server_url,
        McpOAuthTokenRequest request,
        std::string code,
        std::string code_verifier,
        std::string redirect_url);

/// pi `refreshAuthorization`: the refresh grant, with the rotated refresh token
/// merged over the old one so a response that omits it does not lose the grant.
[[nodiscard]] boost::asio::awaitable<support::Expected<McpOAuthTokenResponse>> refresh_authorization(
        std::shared_ptr<ai::auth::OAuthHttpClient> http,
        std::string authorization_server_url,
        McpOAuthTokenRequest request,
        std::string refresh_token);

/// pi `OAuthFlowOptions`, narrowed to what the MCP sign-in and request-time
/// refresh pass: an explicit scope, the authorization code (and its RFC 9207
/// `iss`) when the browser flow has returned, a `WWW-Authenticate`
/// `resource_metadata` URL, and `skip_refresh` for a step-up challenge (a
/// refresh keeps the granted scope, so more scope needs fresh consent).
struct McpOAuthFlowOptions {
    std::optional<std::string> scope;
    std::optional<std::string> authorization_code;
    std::optional<std::string> iss;
    std::optional<std::string> resource_metadata_url;
    bool skip_refresh{false};
};

/// pi `OAuthFlowResult`: the flow either has tokens, or needs the user to
/// authorize and produced the URL to open.
enum class McpOAuthFlowResult {
    Authorized,
    Redirect,
};

struct McpOAuthFlowOutcome {
    McpOAuthFlowResult result{McpOAuthFlowResult::Authorized};
    /// Present for `Redirect`: the authorization URL to open.
    std::optional<std::string> authorization_url;
};

/// pi `authorizeMcp` for one MCP server: discover the authorization server
/// (RFC 9728 / RFC 8414), take its first advertised scope set as the default,
/// reuse the stored client registration or register one (or identify through a
/// Client ID Metadata Document), then exchange the authorization code, refresh
/// the stored tokens, or return the authorization URL. The state it reads and
/// writes is `mcp-auth.json`; `oauth.invalid_client` and
/// `oauth.unauthorized_client` invalidate every stored credential and run once
/// more, and `oauth.invalid_grant` invalidates the tokens and runs once more,
/// so a stale registration or a dead refresh token is corrected without a
/// retry loop.
[[nodiscard]] boost::asio::awaitable<support::Expected<McpOAuthFlowOutcome>> authorize_mcp(
        std::shared_ptr<McpAuthStore> store,
        std::string server_name,
        std::string server_url,
        McpOAuthConfig oauth,
        std::string redirect_url,
        std::shared_ptr<ai::auth::OAuthHttpClient> http,
        McpOAuthFlowOptions options = {});

} // namespace cch::coding_agent::mcp

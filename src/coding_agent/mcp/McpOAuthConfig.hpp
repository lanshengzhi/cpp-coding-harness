#pragma once

// pi `McpOAuthConfig` (spec #882, ticket #884): the `oauth` block of an
// `mcp.json` server entry. pi source at `7c10bd43` (v1.0.4):
// `packages/coding-agent/src/core/mcp-servers.ts` (`McpOAuthConfig`,
// `validateOAuth`, `isLoopbackRedirectUri`) and the frozen declaration in
// `fixtures/pi-ai/v1.0.4/mcp-codemode/mcp-config-surface.json`
// (`declarations.oauthConfig`). The endpoints are not part of this block: pi
// discovers them through the server's protected-resource metadata (RFC 9728).
// A pre-registered client (`clientId`, `clientSecret`, `callbackPort`,
// `callbackUrl`, `scope`, `clientName`) overrides dynamic registration or CIMD.

#include <optional>
#include <string>

namespace cch::coding_agent::mcp {

/// How pi identifies itself to the authorization server when `client_id` is not
/// configured (pi `clientRegistration`).
enum class McpClientRegistration {
    /// `dcr` (default): dynamic client registration at the authorization
    /// server's `registration_endpoint`.
    Dcr,
    /// `cimd`: a Client ID Metadata Document URL instead of registering.
    Cimd,
};

/// pi `McpOAuthConfig`: the `oauth` block of one streamable-HTTP server entry.
/// Every field is optional; the block itself means "this server uses OAuth".
struct McpOAuthConfig {
    /// Pre-registered client id. Without it, pi registers a client
    /// dynamically, or uses a metadata document under `cimd`.
    std::optional<std::string> client_id;
    /// Pre-registered client secret; may name an environment variable or a
    /// command (resolved at sign-in, not here).
    std::optional<std::string> client_secret;
    /// Loopback callback port, for a client registered with a fixed redirect
    /// URI. Without `callback_url` the redirect URI is
    /// `http://127.0.0.1:<port>/callback`.
    std::optional<int> callback_port;
    /// Redirect URI registered for `client_id`: an `http` URI on `localhost`,
    /// `127.0.0.1`, or `[::1]` with no query or fragment (pi
    /// `isLoopbackRedirectUri`).
    std::optional<std::string> callback_url;
    /// Scopes to request, space-separated. Empty means the scopes the server
    /// advertises.
    std::optional<std::string> scope;
    /// pi `client_name` sent with dynamic client registration. Default: `pi`.
    std::optional<std::string> client_name;
    /// pi `clientRegistration` (default `dcr`).
    std::optional<McpClientRegistration> client_registration;
    /// Authorization-server metadata document (RFC 8414 or OpenID Connect
    /// discovery) used instead of discovery through the server.
    std::optional<std::string> auth_server_metadata_url;
};

} // namespace cch::coding_agent::mcp

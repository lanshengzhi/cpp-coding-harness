#pragma once

#include <cch/support/Error.hpp>

#include <cstdint>
#include <string>

namespace cch::coding_agent::mcp {

/// OAuth sign-in configuration for one streamable-HTTP MCP server (spec #865,
/// ticket #875). The credential itself is not stored here: it lives in the
/// shared `auth.json` through AuthStorage under the server's OAuth provider id
/// (`mcp_oauth_provider_id`), so no second credential store exists. This value
/// carries only the authorization-server endpoints and the pre-registered
/// client identity, reusing the existing `ai::OAuthCredential` semantics.
///
/// The shape maps to a plain `mcp.json` server entry gaining an OAuth block
/// (ticket #876): `auth: "oauth"` plus `authorizationUrl`, `tokenUrl`,
/// `clientId`, and `scope`. Dynamic client registration and RFC 9728
/// authorization-server discovery are not part of this slice — the endpoints
/// are configured — and neither is a user-visible sign-in trigger (see the
/// #875 ruling in ADR 0066).
struct McpOAuthServerConfig {
    /// Authorization endpoint. `https://`, or a loopback `http://` endpoint for
    /// a native app's own local redirect (RFC 8252); every other form is
    /// rejected at registration.
    std::string authorization_url;
    /// Token endpoint (authorization-code exchange and refresh); same endpoint
    /// rule.
    std::string token_url;
    /// The pre-registered public client id. Dynamic client registration is not
    /// part of this slice, so the id is required.
    std::string client_id;
    /// Requested scope, space-separated. Empty sends no `scope` parameter.
    std::string scope;
    /// Loopback callback bind host; the frozen default is 127.0.0.1.
    std::string callback_host{"127.0.0.1"};
    /// Loopback callback bind port; 0 lets the OS assign one.
    std::uint16_t callback_port{0};
};

/// Registration-time gate for the OAuth endpoints: both the authorization and
/// token URLs must be `https://`, or a loopback `http://` endpoint (RFC 8252
/// native-app redirect), and the client id must be present. A rejected config
/// is returned as an explicit Validation error; the flow never silently falls
/// back to an unauthenticated request.
[[nodiscard]] support::ExpectedVoid validate_mcp_oauth_server_config(const McpOAuthServerConfig& config);

} // namespace cch::coding_agent::mcp

#pragma once

// OAuth endpoints for one streamable-HTTP MCP server (pi's configured endpoint
// form, spec #865/#875). The credential itself is not stored here; the caller
// supplies an `OAuthStateStore`.

#include <cch/support/Error.hpp>

#include <cstdint>
#include <string>

namespace cch::mcp {

struct OAuthServerConfig {
    /// Authorization endpoint. `https://`, or a loopback `http://` endpoint for
    /// a native app's own local redirect (RFC 8252); every other form is
    /// rejected at registration.
    std::string authorization_url;
    /// Token endpoint (authorization-code exchange and refresh); same endpoint
    /// rule.
    std::string token_url;
    /// The pre-registered public client id.
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
/// native-app redirect), and the client id must be present.
[[nodiscard]] support::ExpectedVoid validate_oauth_server_config(const OAuthServerConfig& config);

} // namespace cch::mcp

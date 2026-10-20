#pragma once

// pi `McpOAuthConfig`: the `oauth` block of one MCP server entry. Every field
// is optional; the block itself means "this server uses OAuth". Endpoints are
// discovered (RFC 9728) or supplied by auth-server metadata, not part of this
// block.

#include <optional>
#include <string>

namespace cch::mcp {

/// How pi identifies itself to the authorization server when `client_id` is not
/// configured (pi `clientRegistration`).
enum class ClientRegistration {
    /// `dcr` (default): dynamic client registration at the authorization
    /// server's `registration_endpoint`.
    Dcr,
    /// `cimd`: a Client ID Metadata Document URL instead of registering.
    Cimd,
};

struct OAuthConfig {
    std::optional<std::string> client_id;
    std::optional<std::string> client_secret;
    std::optional<int> callback_port;
    std::optional<std::string> callback_url;
    std::optional<std::string> scope;
    std::optional<std::string> client_name;
    std::optional<ClientRegistration> client_registration;
    std::optional<std::string> auth_server_metadata_url;
};

} // namespace cch::mcp

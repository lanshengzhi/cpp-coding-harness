// MCP OAuth registration gate (spec #865, ticket #875). The authorization and
// token endpoints come from configuration (endpoint discovery is a later
// slice), so an insecure endpoint is caught here before any login request is
// built: `https://` everywhere, or a loopback `http://` endpoint for a native
// app's local redirect (RFC 8252). A loopback endpoint is the only plaintext
// form the flow accepts, and it is never used to reach a remote host.

#include "coding_agent/mcp/McpOAuthServerConfig.hpp"
#include "coding_agent/mcp/McpUrl.hpp"

#include <cch/support/Error.hpp>

#include <string>
#include <string_view>

namespace cch::coding_agent::mcp {
namespace {

[[nodiscard]] bool is_loopback_host(std::string_view host) {
    if (host == "localhost" || host == "127.0.0.1" || host == "::1" || host == "[::1]") {
        return true;
    }
    return false;
}

/// The authority component of a URL (`scheme://authority/...`), lowercased and
/// stripped of any userinfo and port. Empty when the URL has no authority.
[[nodiscard]] std::string url_host(std::string_view url) { return extract_mcp_host(url); }

[[nodiscard]] support::ExpectedVoid validate_endpoint(std::string_view name, const std::string& url) {
    if (url.starts_with("https://")) {
        if (url_host(url).empty()) {
            return std::unexpected(support::make_error(support::ErrorCode::Validation,
                    "MCP OAuth " + std::string{name} + " is missing a host",
                    "rejected URL: " + url));
        }
        return {};
    }
    if (url.starts_with("http://")) {
        if (is_loopback_host(url_host(url))) {
            return {};
        }
        return std::unexpected(support::make_error(support::ErrorCode::Validation,
                "MCP OAuth " + std::string{name} + " may use http:// only on a loopback host (RFC 8252); use https://",
                "rejected URL: " + url));
    }
    return std::unexpected(support::make_error(support::ErrorCode::Validation,
            "MCP OAuth " + std::string{name} + " must be an https:// URL",
            url.empty() ? "missing URL" : "rejected URL: " + url));
}

} // namespace

support::ExpectedVoid validate_mcp_oauth_server_config(const McpOAuthServerConfig& config) {
    if (auto authorization = validate_endpoint("authorization endpoint", config.authorization_url); !authorization) {
        return authorization;
    }
    if (auto token = validate_endpoint("token endpoint", config.token_url); !token) {
        return token;
    }
    if (config.client_id.empty()) {
        return std::unexpected(support::make_error(support::ErrorCode::Validation,
                "MCP OAuth client id is required",
                "dynamic client registration is not part of this slice"));
    }
    return {};
}

} // namespace cch::coding_agent::mcp

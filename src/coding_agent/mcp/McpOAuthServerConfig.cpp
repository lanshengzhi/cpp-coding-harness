// MCP OAuth registration gate (spec #865, ticket #875). The authorization and
// token endpoints come from configuration (endpoint discovery is a later
// slice), so an insecure endpoint is caught here before any login request is
// built: `https://` everywhere, or a loopback `http://` endpoint for a native
// app's local redirect (RFC 8252). A loopback endpoint is the only plaintext
// form the flow accepts, and it is never used to reach a remote host.

#include "coding_agent/mcp/McpOAuthServerConfig.hpp"

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
[[nodiscard]] std::string url_host(std::string_view url) {
    const auto scheme_separator = url.find("://");
    if (scheme_separator == std::string_view::npos) {
        return {};
    }
    std::string_view authority = url.substr(scheme_separator + 3);
    if (const auto path_start = authority.find('/'); path_start != std::string_view::npos) {
        authority = authority.substr(0, path_start);
    }
    if (const auto userinfo = authority.rfind('@'); userinfo != std::string_view::npos) {
        authority = authority.substr(userinfo + 1);
    }
    // A bracketed IPv6 literal keeps its brackets so `[::1]` compares equal.
    if (!authority.empty() && authority.front() != '[') {
        if (const auto port = authority.rfind(':'); port != std::string_view::npos) {
            authority = authority.substr(0, port);
        }
    }
    std::string host{authority};
    for (char& character : host) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    return host;
}

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

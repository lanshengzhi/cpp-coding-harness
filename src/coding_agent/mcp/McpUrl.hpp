#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace cch::coding_agent::mcp {

/// Parsed URL authority and components for MCP OAuth, configuration, and endpoint validation.
/// Handles:
/// - Case-insensitive scheme and host normalization
/// - Bracketed IPv6 literals (e.g. `[::1]`)
/// - Userinfo stripping (`user:pass@host`)
/// - Port parsing and default port stripping (80 for http, 443 for https)
struct McpParsedUrl {
    bool valid{false};
    std::string scheme;
    std::string host;
    std::string port;
    std::optional<std::uint16_t> port_number;
    std::string path{"/"};
    std::string search;
    std::string hash;
    std::string origin;
    std::string authority;
};

/// Parse a URL string into its components. Returns an invalid `McpParsedUrl` (valid == false)
/// if the URL does not contain a scheme separator `://` or has an empty host.
[[nodiscard]] McpParsedUrl parse_mcp_url(std::string_view value);

/// Extract just the lowercase hostname (with brackets if IPv6) from a URL or authority string.
[[nodiscard]] std::string extract_mcp_host(std::string_view url_or_authority);

} // namespace cch::coding_agent::mcp

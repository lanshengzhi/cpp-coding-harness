#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace cch::mcp {

/// Parsed URL authority and components for MCP OAuth, configuration, and
/// endpoint validation. Handles case-insensitive scheme/host normalization,
/// bracketed IPv6 literals, userinfo stripping, and default port stripping.
struct ParsedUrl {
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

/// Parse a URL string into its components. Returns an invalid `ParsedUrl`
/// (valid == false) if the URL does not contain `://` or has an empty host.
[[nodiscard]] ParsedUrl parse_url(std::string_view value);

/// Extract just the lowercase hostname (with brackets if IPv6) from a URL or
/// authority string.
[[nodiscard]] std::string extract_host(std::string_view url_or_authority);

} // namespace cch::mcp

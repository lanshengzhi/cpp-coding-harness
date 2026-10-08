#include "coding_agent/mcp/McpUrl.hpp"

#include <cctype>
#include <charconv>

namespace cch::coding_agent::mcp {

McpParsedUrl parse_mcp_url(std::string_view value) {
    McpParsedUrl result;
    const auto scheme_end = value.find("://");
    if (scheme_end == std::string_view::npos || scheme_end == 0) {
        return result;
    }

    std::string scheme{value.substr(0, scheme_end)};
    for (char& character : scheme) {
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    result.scheme = std::move(scheme);

    std::string_view rest = value.substr(scheme_end + 3);
    const auto authority_end = rest.find_first_of("/?#");
    std::string_view authority = authority_end == std::string_view::npos ? rest : rest.substr(0, authority_end);
    std::string_view remainder =
            authority_end == std::string_view::npos ? std::string_view{} : rest.substr(authority_end);

    result.authority = std::string{authority};

    if (const auto userinfo = authority.rfind('@'); userinfo != std::string_view::npos) {
        authority = authority.substr(userinfo + 1);
    }
    if (authority.empty()) {
        return result;
    }

    std::string host;
    std::string port;
    if (authority.front() == '[') {
        const auto close = authority.find(']');
        if (close == std::string_view::npos) {
            return result;
        }
        host = std::string{authority.substr(0, close + 1)};
        std::string_view after = authority.substr(close + 1);
        if (after.starts_with(':')) {
            port = std::string{after.substr(1)};
        }
    } else if (const auto colon = authority.rfind(':'); colon != std::string_view::npos) {
        host = std::string{authority.substr(0, colon)};
        port = std::string{authority.substr(colon + 1)};
    } else {
        host = std::string{authority};
    }

    if (host.empty()) {
        return result;
    }

    for (char& character : host) {
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    result.host = std::move(host);

    if (!port.empty()) {
        std::uint16_t parsed_port = 0;
        const auto converted = std::from_chars(port.data(), port.data() + port.size(), parsed_port);
        if (converted.ec == std::errc{} && converted.ptr == port.data() + port.size()) {
            result.port_number = parsed_port;
            result.port = port;
        }
    }

    // Default port handling per URL standard: drop 80 for http, 443 for https
    if ((result.scheme == "https" && result.port == "443") || (result.scheme == "http" && result.port == "80")) {
        result.port.clear();
        result.port_number.reset();
    }

    result.origin = result.scheme + "://" + result.host;
    if (!result.port.empty()) {
        result.origin += ":" + result.port;
    }

    std::string_view path = remainder;
    if (const auto hash_start = path.find('#'); hash_start != std::string_view::npos) {
        result.hash = std::string{path.substr(hash_start)};
        path = path.substr(0, hash_start);
    }
    if (const auto query_start = path.find('?'); query_start != std::string_view::npos) {
        result.search = std::string{path.substr(query_start)};
        path = path.substr(0, query_start);
    }
    result.path = path.empty() ? "/" : std::string{path};
    result.valid = true;
    return result;
}

std::string extract_mcp_host(std::string_view url_or_authority) {
    if (url_or_authority.find("://") != std::string_view::npos) {
        const auto parsed = parse_mcp_url(url_or_authority);
        return parsed.valid ? parsed.host : std::string{};
    }
    // Bare authority
    if (const auto userinfo = url_or_authority.rfind('@'); userinfo != std::string_view::npos) {
        url_or_authority = url_or_authority.substr(userinfo + 1);
    }
    if (const auto path_start = url_or_authority.find_first_of("/?#"); path_start != std::string_view::npos) {
        url_or_authority = url_or_authority.substr(0, path_start);
    }
    std::string host;
    if (!url_or_authority.empty() && url_or_authority.front() == '[') {
        const auto close = url_or_authority.find(']');
        if (close != std::string_view::npos) {
            host = std::string{url_or_authority.substr(0, close + 1)};
        } else {
            host = std::string{url_or_authority};
        }
    } else {
        const auto colon = url_or_authority.rfind(':');
        if (colon != std::string_view::npos) {
            host = std::string{url_or_authority.substr(0, colon)};
        } else {
            host = std::string{url_or_authority};
        }
    }
    for (char& character : host) {
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    return host;
}

} // namespace cch::coding_agent::mcp

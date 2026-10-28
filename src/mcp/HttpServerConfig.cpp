// MCP streamable-http registration gate (pi v1.3.0 `validateMcpServerConfig`,
// ADR 0075): a configured MCP server URL is accepted in its `http://` or
// `https://` form and rejected here at registration for any other scheme,
// with pi's message. Plain HTTP is the frozen upstream behavior; certificate
// validation on HTTPS stays platform-enforced downstream in the transport.

#include <cch/mcp/HttpServerConfig.hpp>

#include <cch/support/Error.hpp>

#include <string>
#include <string_view>

namespace cch::mcp {

support::ExpectedVoid validate_http_server_config(const HttpServerConfig& config) {
    if (config.name.empty()) {
        return std::unexpected(support::make_error(support::ErrorCode::Validation, "MCP server has no name"));
    }
    const bool http = config.url.starts_with("http://");
    const bool https = config.url.starts_with("https://");
    if (!http && !https) {
        return std::unexpected(support::make_error(support::ErrorCode::Validation,
                "MCP server '" + config.name + "' url must be an http or https URL",
                config.url.empty() ? "missing URL" : "rejected URL: " + config.url));
    }
    const std::string_view authority = std::string_view{config.url}.substr(
            https ? std::string_view{"https://"}.size() : std::string_view{"http://"}.size());
    const auto slash = authority.find('/');
    const std::string_view host_port = slash == std::string_view::npos ? authority : authority.substr(0, slash);
    // `http://?query` or `http://#fragment` have an empty host: pi's
    // `new URL(value)` parses them with an empty hostname and is rejected.
    if (host_port.empty() || host_port.front() == '?' || host_port.front() == '#') {
        return std::unexpected(support::make_error(support::ErrorCode::Validation,
                "MCP server '" + config.name + "' URL is missing a host",
                "rejected URL: " + config.url));
    }
    return {};
}

} // namespace cch::mcp

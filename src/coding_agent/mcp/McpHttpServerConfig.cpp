// MCP streamable-http registration gate (spec #865, ticket #873). Client
// transports are TLS-only (ADR 0054): a configured MCP server URL is accepted
// only in its `https://` form, rejected here at registration, and the
// transport never falls back to plaintext.

#include "coding_agent/mcp/McpHttpServerConfig.hpp"

#include <cch/support/Error.hpp>

#include <string>
#include <string_view>

namespace cch::coding_agent::mcp {

support::ExpectedVoid validate_mcp_http_server_config(const McpHttpServerConfig& config) {
    if (config.name.empty()) {
        return std::unexpected(support::make_error(support::ErrorCode::Validation, "MCP server has no name"));
    }
    if (!config.url.starts_with("https://")) {
        return std::unexpected(support::make_error(support::ErrorCode::Validation,
                "MCP server '" + config.name + "' URL must use https:// (TLS-only)",
                config.url.empty() ? "missing URL" : "rejected URL: " + config.url));
    }
    const std::string_view authority = std::string_view{config.url}.substr(std::string_view{"https://"}.size());
    const auto slash = authority.find('/');
    const std::string_view host_port = slash == std::string_view::npos ? authority : authority.substr(0, slash);
    if (host_port.empty()) {
        return std::unexpected(support::make_error(support::ErrorCode::Validation,
                "MCP server '" + config.name + "' URL is missing a host",
                "rejected URL: " + config.url));
    }
    return {};
}

} // namespace cch::coding_agent::mcp

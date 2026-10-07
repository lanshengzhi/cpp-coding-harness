#pragma once

// The one spelling of pi's MCP server namespace (`mcpNamespace`), shared by the
// tool-name namespace (McpConfigFile's collision check) and the OAuth
// credential-store provider id (mcp_oauth_provider_id), which are the same
// value by construction: `mcp__<server>` with every `-` replaced by `_`.

#include <string>
#include <string_view>

namespace cch::coding_agent::mcp::detail {

/// pi `mcpNamespace`: `mcp__<name>` with `-` replaced by `_`. Two names that
/// differ only in `-`/`_` share this namespace and cannot coexist.
[[nodiscard]] inline std::string mcp_namespace(std::string_view name) {
    std::string result = "mcp__";
    result.reserve(name.size() + 5);
    for (const char character : name) {
        result.push_back(character == '-' ? '_' : character);
    }
    return result;
}

} // namespace cch::coding_agent::mcp::detail

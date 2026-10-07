#pragma once

// pi `McpExposure` and the exposure policy that decides how a server's tools
// reach the model (spec #882, ticket #884). pi source at `7c10bd43` (v1.0.4):
// `packages/coding-agent/src/core/mcp-servers.ts` (`McpExposure`,
// `MCP_EXPOSURES`, `MCP_EXPOSURE_ALIASES`, `toolPatternRegExp`,
// `getMcpToolExposure`).

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::coding_agent::mcp {

/// pi `McpExposure`: how a server's tools are exposed to the model.
///
/// - `Codemode`: tools are callable from codemode scripts but neither declared
///   to the model nor listed in the codemode description, which lists only the
///   server's namespace. `codemode-deferred` is accepted as an alias.
/// - `Deferred`: not declared to the model until the `tool_search` tool loads
///   them; the model then calls them directly. Does not need codemode.
/// - `Direct`: tools are declared to the model like any other tool (and
///   callable from codemode).
/// - `Hidden`: tools are registered but unreachable.
enum class McpExposure { Codemode, Deferred, Direct, Hidden };

/// The canonical pi spelling of an exposure (`MCP_EXPOSURES`).
[[nodiscard]] inline std::string_view mcp_exposure_name(McpExposure exposure) {
    switch (exposure) {
    case McpExposure::Codemode:
        return "codemode";
    case McpExposure::Deferred:
        return "deferred";
    case McpExposure::Direct:
        return "direct";
    case McpExposure::Hidden:
        return "hidden";
    }
    return "codemode";
}

/// The pi error fragment listing every accepted exposure spelling
/// (`validateMcpServerConfig`): `"codemode", "deferred", "direct", "hidden"`.
[[nodiscard]] inline std::string mcp_exposure_list() { return "\"codemode\", \"deferred\", \"direct\", \"hidden\""; }

/// Parse one exposure spelling, resolving pi's aliases. `std::nullopt` when the
/// value is not an exposure (`resolveExposureAlias` then `isExposure`).
[[nodiscard]] inline std::optional<McpExposure> parse_mcp_exposure(std::string_view value) {
    if (value == "codemode") {
        return McpExposure::Codemode;
    }
    if (value == "deferred") {
        return McpExposure::Deferred;
    }
    if (value == "direct") {
        return McpExposure::Direct;
    }
    if (value == "hidden") {
        return McpExposure::Hidden;
    }
    // pi `MCP_EXPOSURE_ALIASES`: `codemode-deferred` -> `codemode`.
    if (value == "codemode-deferred") {
        return McpExposure::Codemode;
    }
    return std::nullopt;
}

/// pi `toolPatternRegExp`: `*` matches any characters and every other character
/// is literal. A pattern with no `*` matches only itself.
[[nodiscard]] inline bool mcp_tool_pattern_matches(std::string_view pattern, std::string_view name) {
    std::size_t name_pos = 0;
    std::size_t pattern_pos = 0;
    std::size_t star = std::string_view::npos;
    std::size_t star_match = 0;
    while (name_pos < name.size()) {
        if (pattern_pos < pattern.size() && pattern[pattern_pos] == name[name_pos]) {
            ++pattern_pos;
            ++name_pos;
        } else if (pattern_pos < pattern.size() && pattern[pattern_pos] == '*') {
            star = pattern_pos++;
            star_match = name_pos;
        } else if (star != std::string_view::npos) {
            pattern_pos = star + 1;
            name_pos = ++star_match;
        } else {
            return false;
        }
    }
    while (pattern_pos < pattern.size() && pattern[pattern_pos] == '*') {
        ++pattern_pos;
    }
    return pattern_pos == pattern.size();
}

/// pi `getMcpToolExposure`: the tool's `toolExposure` entry, else the server's
/// `exposure`, else `codemode`. An exact name wins over patterns; among
/// patterns the first match in declaration order wins, so `tool_exposure` keeps
/// its declared order.
[[nodiscard]] inline McpExposure get_mcp_tool_exposure(
        const std::vector<std::pair<std::string, McpExposure>>& tool_exposure,
        std::optional<McpExposure> server_exposure,
        std::string_view tool_name) {
    for (const auto& [name, exposure] : tool_exposure) {
        if (name == tool_name) {
            return exposure;
        }
    }
    for (const auto& [pattern, exposure] : tool_exposure) {
        if (pattern.find('*') != std::string::npos && mcp_tool_pattern_matches(pattern, tool_name)) {
            return exposure;
        }
    }
    return server_exposure.value_or(McpExposure::Codemode);
}

} // namespace cch::coding_agent::mcp

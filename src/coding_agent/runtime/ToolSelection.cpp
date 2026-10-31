#include "coding_agent/runtime/ToolSelection.hpp"

#include "coding_agent/mcp/McpNamespace.hpp"
#include "coding_agent/runtime/ToolNames.hpp"

#include <algorithm>
#include <cstddef>

namespace cch::coding_agent::runtime {

namespace {

/// Anchored glob match equivalent to pi's `toolPatternRegExp` (a `*` becomes
/// `.*`, everything else is matched literally, and the whole name is
/// anchored): the classic wildcard scan where `*` matches any run including
/// the empty one.
[[nodiscard]] bool wildcard_matches(std::string_view pattern, std::string_view name) {
    std::size_t pattern_index = 0;
    std::size_t name_index = 0;
    std::size_t star_pattern_index = std::string_view::npos;
    std::size_t star_name_index = 0;
    while (name_index < name.size()) {
        if (pattern_index < pattern.size() && pattern[pattern_index] == '*') {
            star_pattern_index = pattern_index++;
            star_name_index = name_index;
        } else if (pattern_index < pattern.size() && pattern[pattern_index] == name[name_index]) {
            ++pattern_index;
            ++name_index;
        } else if (star_pattern_index != std::string_view::npos) {
            pattern_index = star_pattern_index + 1;
            name_index = ++star_name_index;
        } else {
            return false;
        }
    }
    while (pattern_index < pattern.size() && pattern[pattern_index] == '*') {
        ++pattern_index;
    }
    return pattern_index == pattern.size();
}

[[nodiscard]] bool contains_mcp_entry(const std::vector<std::string>& entries) {
    return std::ranges::any_of(entries, [](const std::string& entry) { return entry.starts_with("mcp__"); });
}

/// pi `getToolListError`: a list is either plain names/patterns or only exact
/// modifiers; mixing the forms or using a wildcard modifier is invalid.
[[nodiscard]] std::optional<std::string> tool_list_error(std::span<const std::string> entries) {
    const auto modifiers =
            std::ranges::count_if(entries, [](const std::string& entry) { return is_tool_modifier(entry); });
    if (modifiers == 0) {
        return std::nullopt;
    }
    if (static_cast<std::size_t>(modifiers) < entries.size()) {
        return std::string{"tool names cannot be mixed with +name or -name entries"};
    }
    const auto pattern = std::ranges::find_if(
            entries, [](const std::string& entry) { return entry.find('*') != std::string::npos; });
    if (pattern != entries.end()) {
        return "+name and -name entries take exact tool names, not patterns: " + *pattern;
    }
    return std::nullopt;
}

/// pi `_allowlistFiltersMcp`: an engaged empty allowlist, or one that names an
/// `mcp__` entry, subjects MCP tools to the allowlist; otherwise MCP tools are
/// kept even though they are not named.
[[nodiscard]] bool allowlist_filters_mcp(const std::vector<std::string>& allowed) {
    return allowed.empty() || contains_mcp_entry(allowed);
}

[[nodiscard]] bool pattern_matches_any(std::string_view pattern, std::span<const std::string> discovered_names) {
    return std::ranges::any_of(
            discovered_names, [pattern](const std::string& name) { return wildcard_matches(pattern, name); });
}

[[nodiscard]] support::Error unmatched_pattern_error(
        std::string_view entry, std::span<const std::string> discovered_names) {
    return support::make_error(support::ErrorCode::Validation,
            "tool pattern '" + std::string{entry} + "' matched no discovered tool",
            "discovered tools: " + detail::join_tool_names(discovered_names));
}

[[nodiscard]] support::ExpectedVoid validate_patterns(
        const std::vector<std::string>& entries, std::span<const std::string> discovered_names) {
    for (const auto& entry : entries) {
        if (entry.find('*') != std::string::npos && !pattern_matches_any(entry, discovered_names)) {
            return std::unexpected(unmatched_pattern_error(entry, discovered_names));
        }
    }
    return {};
}

} // namespace

bool tool_name_matches(std::span<const std::string> entries, std::string_view name) {
    for (const auto& entry : entries) {
        if (entry.find('*') == std::string::npos) {
            if (entry == name) {
                return true;
            }
        } else if (wildcard_matches(entry, name)) {
            return true;
        }
    }
    return false;
}

bool is_mcp_tool_name(std::string_view name) {
    return name.starts_with("mcp__") || name == mcp::kListMcpResourcesTool ||
           name == mcp::kListMcpResourceTemplatesTool || name == mcp::kReadMcpResourceTool;
}

bool is_tool_modifier(std::string_view entry) { return entry.starts_with('+') || entry.starts_with('-'); }

support::Expected<ToolSelection> make_tool_selection(
        std::optional<std::vector<std::string>> tools, std::vector<std::string> excluded) {
    if (tools.has_value()) {
        if (auto problem = tool_list_error(*tools)) {
            return std::unexpected(support::make_error(
                    support::ErrorCode::Validation, "Invalid tools option: " + std::move(*problem)));
        }
        if (std::ranges::any_of(*tools, [](const std::string& entry) { return is_tool_modifier(entry); })) {
            ToolSelection selection;
            selection.excluded = std::move(excluded);
            selection.modifiers = std::move(*tools);
            return selection;
        }
    }
    ToolSelection selection;
    selection.allowed = std::move(tools);
    selection.excluded = std::move(excluded);
    return selection;
}

std::vector<std::string> apply_tool_modifiers(std::span<const std::string> base, std::span<const std::string> entries) {
    std::vector<std::string> tools{base.begin(), base.end()};
    for (const auto& entry : entries) {
        if (!is_tool_modifier(entry)) {
            continue;
        }
        const std::string name = entry.substr(1);
        const auto existing = std::ranges::find(tools, name);
        if (entry.starts_with('+')) {
            if (existing == tools.end() && !name.empty()) {
                tools.push_back(name);
            }
        } else if (existing != tools.end()) {
            tools.erase(existing);
        }
    }
    return tools;
}

support::Expected<ToolSelectionResult> resolve_tool_selection(
        const ToolSelection& selection, std::span<const std::string> discovered_names) {
    if (selection.allowed) {
        if (auto valid = validate_patterns(*selection.allowed, discovered_names); !valid) {
            return std::unexpected(std::move(valid.error()));
        }
    }
    if (auto valid = validate_patterns(selection.excluded, discovered_names); !valid) {
        return std::unexpected(std::move(valid.error()));
    }

    const bool allowlist_active = selection.allowed.has_value();
    const bool filters_mcp = allowlist_active && allowlist_filters_mcp(*selection.allowed);

    ToolSelectionResult result;
    for (const auto& name : discovered_names) {
        bool kept = true;
        if (tool_name_matches(selection.excluded, name)) {
            kept = false;
        } else if (allowlist_active && !tool_name_matches(*selection.allowed, name)) {
            kept = !filters_mcp && is_mcp_tool_name(name);
        }
        if (kept) {
            result.selected.push_back(name);
        } else {
            result.removed.push_back(name);
        }
    }
    return result;
}

} // namespace cch::coding_agent::runtime

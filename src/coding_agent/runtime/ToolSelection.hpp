#pragma once

#include <cch/support/Error.hpp>

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cch::coding_agent::runtime {

/// One CLI tool-selection intent (pi `--tools` / `--exclude-tools`). Exact
/// names and `*` wildcard patterns share one entry list, exactly like pi's
/// `createToolNameMatcher` entries: a `*` matches any run of characters and the
/// entry is otherwise matched literally.
struct ToolSelection {
    /// pi `--tools`: `std::nullopt` when the flag is absent (every discovered
    /// tool stays unless excluded); an engaged value is the allowlist, and an
    /// engaged empty list allows nothing (pi `options.tools = []`).
    std::optional<std::vector<std::string>> allowed;
    /// pi `--exclude-tools`: an empty list excludes nothing.
    std::vector<std::string> excluded;
    /// pi `--tools` relative `+name`/`-name` entries. When present they change
    /// the default selection on a fresh session and the recorded loadout on a
    /// resumed one, instead of replacing it, so `allowed` stays unset.
    std::vector<std::string> modifiers;

    [[nodiscard]] bool empty() const { return !allowed.has_value() && excluded.empty() && modifiers.empty(); }
};

/// Whether an entry is a pi `+name` or `-name` modifier.
[[nodiscard]] bool is_tool_modifier(std::string_view entry);

/// Validate and split pi's `--tools`/SDK `tools` list: either plain names and
/// patterns, or only exact `+name`/`-name` entries. A mixed list and a wildcard
/// modifier are Validation errors carrying pi's `getToolListError` text.
[[nodiscard]] support::Expected<ToolSelection> make_tool_selection(
        std::optional<std::vector<std::string>> tools, std::vector<std::string> excluded);

/// pi `applyToolModifiers`: apply `+name`/`-name` entries to `base` in entry
/// order. `+name` appends a name that is absent; `-name` removes it, preserving
/// the positions of every other name. Non-modifier entries are ignored.
[[nodiscard]] std::vector<std::string> apply_tool_modifiers(
        std::span<const std::string> base, std::span<const std::string> entries);

/// pi `createToolNameMatcher`: whether `name` matches one entry exactly (an
/// entry without `*`) or as a `*` wildcard pattern.
[[nodiscard]] bool tool_name_matches(std::span<const std::string> entries, std::string_view name);

/// pi `isMcpToolName`: a server tool named `mcp__<server>__<tool>` or one of
/// the three MCP resource tool names (`list_mcp_resources`,
/// `list_mcp_resource_templates`, `read_mcp_resource`).
[[nodiscard]] bool is_mcp_tool_name(std::string_view name);

/// The outcome of resolving a selection over the discovered tool set.
struct ToolSelectionResult {
    /// Names kept, in the discovered set's order.
    std::vector<std::string> selected;
    /// Names removed, in the discovered set's order.
    std::vector<std::string> removed;
};

/// Resolve `selection` over the discovered tool names, applying pi
/// `AgentSession._isAllowedTool`: an excluded name is never kept; otherwise an
/// allowlist match keeps the name; otherwise an MCP tool stays when the
/// allowlist does not name MCP (pi `_allowlistFiltersMcp`). A wildcard entry
/// that matches no discovered tool is a Validation error, so a typo is an
/// explicit failure rather than a silent empty set.
[[nodiscard]] support::Expected<ToolSelectionResult> resolve_tool_selection(
        const ToolSelection& selection, std::span<const std::string> discovered_names);

} // namespace cch::coding_agent::runtime

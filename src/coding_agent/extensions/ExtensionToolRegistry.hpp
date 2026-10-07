#pragma once

#include "coding_agent/extensions/ExtensionTool.hpp"
#include "coding_agent/extensions/ExtensionToolSource.hpp"

#include <cch/agent/ToolRegistry.hpp>
#include <cch/support/Error.hpp>

#include <cstddef>
#include <map>
#include <span>
#include <string>
#include <vector>

namespace cch::coding_agent::extensions {

/// Move-only registry of extension-provided Tools keyed by tool name. This is
/// where extension contributions are collected (the Extension Tool Source's
/// "registry"); SessionFactory's runner converts it into the Agent's
/// ToolRegistry so extension tools share the built-ins' execution path. Tools
/// are held in name order so a loadout's conversion and its collision
/// diagnostics are deterministic.
class ExtensionToolRegistry {
public:
    ExtensionToolRegistry() = default;
    ExtensionToolRegistry(ExtensionToolRegistry&&) noexcept = default;
    ExtensionToolRegistry& operator=(ExtensionToolRegistry&&) noexcept = default;
    ExtensionToolRegistry(const ExtensionToolRegistry&) = delete;
    ExtensionToolRegistry& operator=(const ExtensionToolRegistry&) = delete;

    /// Register one extension Tool. Rejects a missing execute operation, a
    /// missing name, and a duplicate name with a typed Validation error.
    [[nodiscard]] support::ExpectedVoid add(ExtensionTool tool);

    [[nodiscard]] ExtensionTool* find(const std::string& name);
    [[nodiscard]] const ExtensionTool* find(const std::string& name) const;
    [[nodiscard]] std::size_t size() const { return tools_.size(); }
    [[nodiscard]] bool empty() const { return tools_.empty(); }

    /// Remove and return every registered Tool in name order. The runner
    /// consumes the registry this way so the move-only execute operations
    /// transfer exactly once; the registry is empty afterwards.
    [[nodiscard]] std::vector<ExtensionTool> take_tools();

    /// The names registered with pi `defaultActive: false` (registration
    /// without declaration), in registration order. Session Assembly uses the
    /// list to shape the Agent's initial declared set; a name later removed
    /// from the registry (an explicit `--tools` selection is pi's activation
    /// path) drops from this list too.
    [[nodiscard]] const std::vector<std::string>& default_inactive_tool_names() const noexcept {
        return default_inactive_;
    }

private:
    std::map<std::string, ExtensionTool, std::less<>> tools_;
    std::vector<std::string> default_inactive_;
};

/// Loader: fill `registry` from every source in order. A source failure stops
/// the load and is returned unchanged; a duplicate tool name is rejected by
/// the registry's `add`.
[[nodiscard]] support::ExpectedVoid load_extension_tools(
        ExtensionToolRegistry& registry, std::span<ExtensionToolSource* const> sources);

/// Runner: convert each registered extension Tool into a `cch::agent::Tool`
/// and add it to the Agent's ToolRegistry. `extension_tools` is consumed. A
/// name already present in `registry` (a built-in, a custom tool, or an
/// earlier extension tool) is a typed Validation error rather than a silent
/// replacement.
[[nodiscard]] support::ExpectedVoid register_extension_tools(
        agent::ToolRegistry& registry, ExtensionToolRegistry extension_tools);

} // namespace cch::coding_agent::extensions

#pragma once

// pi `ToolExposure` and `ToolNamespace` (`core/extensions/types.ts`, pi-v1.3.0
// `71023aec`): how the model reaches a tool, and the group a tool belongs to.
// The application keeps both as passive values so the codemode catalog and the
// tool-search/discovery surfaces present the same loadout the Agent executes.

#include <string>
#include <string_view>
#include <vector>

namespace cch::coding_agent::extensions {

/// pi `ToolExposure`: how the model reaches a tool. `Callable` means callable
/// from other tools through the nested-call seam (`ctx.executeTool()`), as the
/// `codemode` tool does.
///
/// - `Direct`: declared to the model while active, and callable while active.
/// - `ModelOnly`: declared to the model while active, never callable. Used for
///   orchestrating or interactive tools.
/// - `Codemode`: callable whenever registered. Not declared to the model
///   unless explicitly activated. Codemode tools list it in their description.
/// - `Deferred`: like `Codemode`, but codemode tools do not list it; tool search
///   can find it.
/// - `Hidden`: registered but unreachable. Activating it has no effect.
///
/// `direct` and `model-only` tools are activated when they are registered; the
/// others are not.
enum class ToolExposure {
    Direct,
    ModelOnly,
    Codemode,
    Deferred,
    Hidden,
};

/// The canonical pi spelling of an exposure (`ToolExposure`).
[[nodiscard]] inline std::string_view tool_exposure_name(ToolExposure exposure) {
    switch (exposure) {
    case ToolExposure::Direct:
        return "direct";
    case ToolExposure::ModelOnly:
        return "model-only";
    case ToolExposure::Codemode:
        return "codemode";
    case ToolExposure::Deferred:
        return "deferred";
    case ToolExposure::Hidden:
        return "hidden";
    }
    return "direct";
}

/// pi `ToolNamespace`: a group of related tools, such as the tools of one MCP
/// server. Codemode tools list them together.
struct ToolNamespace {
    /// For example `mcp__docs`.
    std::string name;
    /// Other names that refer to the namespace where tools look it up by name
    /// (codemode's `describeNamespace()` and the `namespace` option of
    /// `searchTools()`), for example `docs`.
    std::vector<std::string> aliases;
    /// Short summary shown once with the group in model-facing tool listings.
    std::string description;
    /// Longer usage guidance, such as MCP server instructions. Not part of tool
    /// listings; tools that describe the namespace on request (codemode's
    /// `describeNamespace()`) return it.
    std::string instructions;
};

} // namespace cch::coding_agent::extensions

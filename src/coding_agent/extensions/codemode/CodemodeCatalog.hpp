#pragma once

// The model-facing `codemode` catalog and loadout (pi `createCodemodeDescription`,
// `toCodemodeDeclaration`, `renderToolSection`, `selectCatalog`,
// `describeScriptCall`, `prepareCodemodeLoadout` in
// `packages/coding-agent/src/extensions/codemode/tool.ts`, pi-v1.3.0
// `71023aec`).
//
// The catalog is the application's answer to "what can a script call": it
// renders the live loadout (settings, exposure, namespace, prompt guidelines
// and deferred discovery) into the `codemode` tool description, and derives the
// per-tool script-call descriptions and hidden declarations the request needs.
// It never invents a tool: every listed entry comes from a registered tool the
// Agent executes, and every advertised global comes from the discovery surface
// the sandbox binds.

#include "coding_agent/extensions/ToolExposure.hpp"

#include <cch/ai/Tool.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace cch::coding_agent::extensions {

/// One tool as pi's `ToolLoadout` sees it: the declaration plus the exposure,
/// namespace and prompt guidelines the model-facing presentation needs.
struct LoadoutTool {
    ai::Tool definition;
    ToolExposure exposure{ToolExposure::Direct};
    std::optional<ToolNamespace> name_space{};
    std::vector<std::string> guidelines{};
};

/// pi `ToolLoadout`: the tools declared to the model and the tools callable
/// through the nested-call seam, both in the session's order.
struct CodemodeLoadout {
    /// Tools declared to the model (the active tools), with their original
    /// descriptions.
    std::vector<ai::Tool> declared;
    /// Tools callable through the nested-call seam.
    std::vector<LoadoutTool> callable;
};

/// pi `ToolLoadoutChanges`: what the loadout hook changes for the next request.
struct CodemodeLoadoutChanges {
    /// Model-facing descriptions of declared tools, by tool name.
    std::map<std::string, std::string, std::less<>> descriptions;
    /// Declared tools whose declarations requests leave out. They stay active
    /// and callable, and the transcript still declares them.
    std::vector<std::string> hidden_declarations;
};

/// The discovery globals every codemode sandbox binds (pi
/// `createDiscoveryGlobals`). The catalog and the sandbox share this list, so a
/// global the description advertises is always one the script can call.
[[nodiscard]] inline std::vector<std::string> codemode_discovery_globals() {
    return {"searchTools", "describeTool", "describeNamespace"};
}

/// The `codemode` settings the catalog consumes (pi `CodemodeSettings` plus the
/// bound-globals capability gate).
struct CodemodeCatalogSettings {
    /// pi `codemode.mode`: `"on"` (default) or `"only"`.
    std::string mode{"on"};
    /// pi `codemode.inlineBudget`; `std::nullopt` uses
    /// `kCodemodeDefaultInlineBudget`.
    std::optional<std::int64_t> inline_budget{std::nullopt};
    /// Every global the sandbox binds. The `models` line appears only when
    /// `models` is among them, so the description can never advertise a global
    /// the sandbox lacks. Pike retains no codemode `models.*` provider
    /// execution, so production binds the discovery globals only.
    std::vector<std::string> bound_globals{codemode_discovery_globals()};

    /// Whether the session binds the `models` namespace (pi
    /// `describeGlobals(models)`).
    [[nodiscard]] bool models() const { return std::ranges::find(bound_globals, "models") != bound_globals.end(); }
};

/// What a script sees of one tool (pi `toCodemodeDeclaration`): its description
/// followed by its prompt guidelines, its input schema, and its output schema
/// with pi's `{ type: "string" }` default.
struct CodemodeDeclaration {
    std::string name;
    std::string description;
    support::JsonValue input_schema;
    support::JsonValue output_schema;
};

/// pi `toCodemodeDeclaration`: tools without an output schema resolve to their
/// text output, so the declaration defaults the output schema to a string.
[[nodiscard]] CodemodeDeclaration to_codemode_declaration(
        const ai::Tool& tool, const std::vector<std::string>& guidelines = {});

/// pi `getCodemodeCallableTools`: every given tool except the codemode tool
/// itself (scripts must not start other scripts).
[[nodiscard]] std::vector<LoadoutTool> callable_tools(const std::vector<LoadoutTool>& tools);

/// pi `describeFields`: the field names of an object schema with the fields of
/// object-typed fields `depth` levels deep, required fields unmarked and
/// optional ones suffixed with `?`. `std::nullopt` for other schemas.
[[nodiscard]] std::optional<std::string> describe_fields(const support::JsonValue& schema, std::size_t depth);

/// pi `describeOutput`: what a script call resolves to, in one line — `a string`,
/// the field names of an object, or the rendered type for anything else.
[[nodiscard]] std::string describe_output(const support::JsonValue& output_schema);

/// pi `describeScriptCall`: a declared tool's description followed by how
/// scripts call it and what the call resolves to.
[[nodiscard]] std::string describe_script_call(const ai::Tool& tool);

/// pi `isNamespaceName`: whether `query` names the namespace, by name or alias,
/// as written or as the same script identifier.
[[nodiscard]] bool is_namespace_name(const ToolNamespace& name_space, std::string_view query);

/// pi `createCodemodeDescription`: the helper list, the discovery globals, and
/// one section per listed tool grouped by namespace. Deferred tools are never
/// listed and do not affect the description; sections are limited to the inline
/// budget.
[[nodiscard]] std::string create_codemode_description(
        const std::vector<LoadoutTool>& tools, const CodemodeCatalogSettings& settings);

/// pi `prepareCodemodeLoadout`: the descriptions and hidden declarations for the
/// session's loadout under the `on`/`only` presentation mode.
[[nodiscard]] CodemodeLoadoutChanges prepare_codemode_loadout(
        const CodemodeLoadout& loadout, const CodemodeCatalogSettings& settings);

} // namespace cch::coding_agent::extensions

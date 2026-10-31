#pragma once

// The live codemode catalog (pi `AgentSession`'s `prepareLoadout` hook plus the
// `CodemodeToolOptions` closures `getToolNamespace`/`getToolGuidelines`/
// `getMode`/`getInlineBudget`).
//
// The state owns the loadout metadata the Agent's passive `Tool` values do not
// carry — exposure, namespace and prompt guidelines — and the original
// descriptions, so a refresh always recomputes from registration facts instead
// of reading back a description it already changed. The session applies the
// result before the first request and again whenever the live tool surface
// changes (an MCP server registering, withdrawing, or re-exposing tools).

#include "coding_agent/extensions/ToolExposure.hpp"
#include "coding_agent/extensions/codemode/CodemodeCatalog.hpp"

#include <cch/agent/Agent.hpp>
#include <cch/agent/ToolRegistry.hpp>
#include <cch/ai/Tool.hpp>

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cch::coding_agent::extensions {

class CodemodeCatalogState {
public:
    explicit CodemodeCatalogState(CodemodeCatalogSettings settings = {});

    [[nodiscard]] const CodemodeCatalogSettings& settings() const noexcept { return settings_; }
    void set_settings(CodemodeCatalogSettings settings);

    /// Add or replace one registered tool's loadout metadata.
    void register_tool(LoadoutTool tool);
    /// Forget one tool (pi never unregisters; a withdrawn tool re-registers as
    /// `hidden`, which the caller does through `register_tool`).
    void remove_tool(std::string_view name);
    [[nodiscard]] const LoadoutTool* find(std::string_view name) const;
    [[nodiscard]] const std::map<std::string, LoadoutTool, std::less<>>& tools() const noexcept { return tools_; }

    /// pi `_getCallableTools`: the `codemode` and `deferred` exposures plus the
    /// declared `direct` tools. `model-only` and `hidden` tools are never
    /// callable, so the catalog never lists them.
    [[nodiscard]] std::vector<LoadoutTool> callable(const std::vector<std::string>& declared_names) const;

    /// The same set for the declared names of the most recent `apply`, so a
    /// script's tool surface matches the catalog that was presented with it.
    [[nodiscard]] std::vector<LoadoutTool> callable() const;

    /// Recompute the catalog for `declared_names` and write every managed
    /// tool's model-facing description. Used before the Agent exists (the
    /// registry is still owned by assembly).
    [[nodiscard]] CodemodeLoadoutChanges apply(
            agent::ToolRegistry& registry, const std::vector<std::string>& declared_names);

    /// Recompute the catalog for the Agent's live declared set and write the
    /// descriptions.
    [[nodiscard]] CodemodeLoadoutChanges apply(agent::Agent& agent);

    /// pi `_hiddenDeclarations`: declared tools whose declarations requests
    /// leave out (`codemode.mode: "only"`). They stay active and callable, and
    /// the transcript still declares them.
    [[nodiscard]] const std::vector<std::string>& hidden_declarations() const noexcept { return hidden_; }

private:
    [[nodiscard]] CodemodeLoadout loadout(const std::vector<std::string>& declared_names) const;
    [[nodiscard]] CodemodeLoadoutChanges compute(const std::vector<std::string>& declared_names) const;

    CodemodeCatalogSettings settings_;
    std::map<std::string, LoadoutTool, std::less<>> tools_;
    std::vector<std::string> declared_names_;
    std::vector<std::string> hidden_;
};

} // namespace cch::coding_agent::extensions

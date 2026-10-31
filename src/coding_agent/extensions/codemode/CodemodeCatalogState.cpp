#include "coding_agent/extensions/codemode/CodemodeCatalogState.hpp"

#include <algorithm>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace cch::coding_agent::extensions {

CodemodeCatalogState::CodemodeCatalogState(CodemodeCatalogSettings settings) : settings_(std::move(settings)) {}

void CodemodeCatalogState::set_settings(CodemodeCatalogSettings settings) { settings_ = std::move(settings); }

void CodemodeCatalogState::register_tool(LoadoutTool tool) {
    const std::string name = tool.definition.name;
    tools_.insert_or_assign(name, std::move(tool));
}

void CodemodeCatalogState::remove_tool(std::string_view name) { tools_.erase(std::string{name}); }

const LoadoutTool* CodemodeCatalogState::find(std::string_view name) const {
    const auto found = tools_.find(std::string{name});
    return found == tools_.end() ? nullptr : &found->second;
}

std::vector<LoadoutTool> CodemodeCatalogState::callable(const std::vector<std::string>& declared_names) const {
    std::set<std::string, std::less<>> declared;
    declared.insert(declared_names.begin(), declared_names.end());
    std::vector<LoadoutTool> callable;
    callable.reserve(tools_.size());
    for (const auto& [name, tool] : tools_) {
        switch (tool.exposure) {
        case ToolExposure::Codemode:
        case ToolExposure::Deferred:
            callable.push_back(tool);
            break;
        case ToolExposure::Direct:
            if (declared.contains(name)) callable.push_back(tool);
            break;
        case ToolExposure::ModelOnly:
        case ToolExposure::Hidden:
            break;
        }
    }
    return callable;
}

std::vector<LoadoutTool> CodemodeCatalogState::callable() const { return callable(declared_names_); }

CodemodeLoadout CodemodeCatalogState::loadout(const std::vector<std::string>& declared_names) const {
    CodemodeLoadout loadout;
    loadout.declared.reserve(declared_names.size());
    for (const auto& name : declared_names) {
        const auto found = tools_.find(name);
        if (found == tools_.end()) continue;
        // pi `loadout.declared` carries the original descriptions, never a
        // description this catalog already rewrote.
        loadout.declared.push_back(found->second.definition);
    }
    loadout.callable = callable(declared_names);
    return loadout;
}

CodemodeLoadoutChanges CodemodeCatalogState::compute(const std::vector<std::string>& declared_names) const {
    return prepare_codemode_loadout(loadout(declared_names), settings_);
}

CodemodeLoadoutChanges CodemodeCatalogState::apply(
        agent::ToolRegistry& registry, const std::vector<std::string>& declared_names) {
    declared_names_ = declared_names;
    auto changes = compute(declared_names);
    // Restore or apply every managed description, so switching between `on`
    // and `only` (a settings reload) is idempotent instead of appending the
    // script-call sentence twice.
    for (const auto& [name, tool] : tools_) {
        const auto described = changes.descriptions.find(name);
        const std::string& description =
                described == changes.descriptions.end() ? tool.definition.description : described->second;
        static_cast<void>(registry.update_description(name, description));
    }
    hidden_ = changes.hidden_declarations;
    return changes;
}

CodemodeLoadoutChanges CodemodeCatalogState::apply(agent::Agent& agent) {
    declared_names_ = agent.active_tools();
    auto changes = compute(declared_names_);
    for (const auto& [name, tool] : tools_) {
        const auto described = changes.descriptions.find(name);
        const std::string& description =
                described == changes.descriptions.end() ? tool.definition.description : described->second;
        static_cast<void>(agent.update_tool_description(name, description));
    }
    hidden_ = changes.hidden_declarations;
    return changes;
}

} // namespace cch::coding_agent::extensions

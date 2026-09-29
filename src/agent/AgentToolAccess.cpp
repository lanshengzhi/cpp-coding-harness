#include "agent/AgentToolAccess.hpp"

#include "agent/AgentImpl.hpp"

#include <algorithm>
#include <string_view>
#include <utility>

namespace cch::agent::detail {

support::ExpectedVoid AgentToolAccess::add_tool(Agent& agent, Tool tool) {
    if (!agent.impl_) {
        return std::unexpected(
                support::make_error(support::ErrorCode::Validation, "cannot add a tool to an empty Agent"));
    }
    if (tool.definition.name.empty()) {
        return std::unexpected(support::make_error(support::ErrorCode::Validation, "cannot add a tool without a name"));
    }
    auto& registry = agent.impl_->run_policy.registry;
    if (registry.find(tool.definition.name) != nullptr) {
        return std::unexpected(support::make_error(
                support::ErrorCode::Validation, "the tool is already registered", tool.definition.name));
    }
    const std::string name = tool.definition.name;
    if (auto added = registry.add(std::move(tool)); !added) {
        return added;
    }
    // `active_tool_names` is the construction path's name-sorted view of the
    // registry's definitions. Inserting in order keeps that invariant so the
    // projection cannot report one loadout while the model is sent another.
    auto& names = agent.impl_->state.active_tool_names;
    const auto position = std::ranges::upper_bound(names, name);
    names.insert(position, name);
    return {};
}

support::ExpectedVoid AgentToolAccess::remove_tool(Agent& agent, std::string_view name) {
    if (!agent.impl_) {
        return std::unexpected(
                support::make_error(support::ErrorCode::Validation, "cannot remove a tool from an empty Agent"));
    }
    if (name.empty()) {
        return std::unexpected(
                support::make_error(support::ErrorCode::Validation, "cannot remove a tool without a name"));
    }
    auto& names = agent.impl_->state.active_tool_names;
    // A name the registry does not hold is a no-op success, and the reported
    // loadout is left exactly as it was.
    if (!agent.impl_->run_policy.registry.remove(name)) {
        return {};
    }
    std::erase(names, name);
    return {};
}

} // namespace cch::agent::detail

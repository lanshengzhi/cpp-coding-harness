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

} // namespace cch::agent::detail

#include <cch/agent/ToolLoadout.hpp>

#include <cch/ai/Timestamps.hpp>
#include "support/Json.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <variant>

namespace cch::agent {
namespace {

/// Fold one system message's tool delta into a declared state (pi
/// `getCurrentTools`): removals first, then additions, and a redeclared name
/// keeps its first position.
void apply_tool_delta(std::vector<ai::Tool>& tools, const ai::SystemMessage& message) {
    for (const auto& reference : message.tools_removed) {
        std::erase_if(tools, [&reference](const ai::Tool& tool) { return tool.name == reference.name; });
    }
    for (const auto& tool : message.tools_added) {
        const auto existing = std::ranges::find(tools, tool.name, &ai::Tool::name);
        if (existing == tools.end()) {
            tools.push_back(tool);
        } else {
            *existing = tool;
        }
    }
}

[[nodiscard]] const ai::Tool* find_tool(const std::vector<ai::Tool>& tools, const std::string& name) {
    const auto found = std::ranges::find(tools, name, &ai::Tool::name);
    return found == tools.end() ? nullptr : &*found;
}

[[nodiscard]] bool same_parameters(const ai::Tool& left, const ai::Tool& right) {
    auto left_json = support::canonical_json(left.parameters);
    auto right_json = support::canonical_json(right.parameters);
    if (!left_json || !right_json) {
        // An unserializable schema cannot be compared structurally; treat it as
        // changed so the transcript re-declares the tool rather than silently
        // keeping a stale declaration.
        return false;
    }
    return *left_json == *right_json;
}

} // namespace

ai::Tool tool_declaration(const ai::Tool& tool) {
    ai::Tool declaration = tool;
    declaration.output_schema = std::nullopt;
    return declaration;
}

bool declarations_equal(const ai::Tool& left, const ai::Tool& right) {
    if (left.name != right.name || left.description != right.description) {
        return false;
    }
    if (left.constrained_sampling.has_value() != right.constrained_sampling.has_value()) {
        return false;
    }
    if (left.constrained_sampling.has_value() &&
            left.constrained_sampling->variants != right.constrained_sampling->variants) {
        return false;
    }
    return same_parameters(left, right);
}

ToolStateChanges tool_state_changes(const std::vector<ai::Tool>& previous, const std::vector<ai::Tool>& current) {
    ToolStateChanges changes;
    for (const auto& tool : current) {
        const auto* previous_tool = find_tool(previous, tool.name);
        if (previous_tool == nullptr || !declarations_equal(*previous_tool, tool)) {
            changes.tools_added.push_back(tool_declaration(tool));
        }
    }
    for (const auto& tool : previous) {
        const auto* current_tool = find_tool(current, tool.name);
        if (current_tool == nullptr || !declarations_equal(tool, *current_tool)) {
            changes.tools_removed.push_back(ai::ToolReference{.name = tool.name});
        }
    }
    return changes;
}

std::vector<ai::Tool> current_tools(const std::vector<ai::MessageVariant>& messages) {
    std::vector<ai::Tool> tools;
    for (const auto& message : messages) {
        if (const auto* system = std::get_if<ai::SystemMessage>(&message)) {
            apply_tool_delta(tools, *system);
        }
    }
    return tools;
}

std::vector<ai::MessageVariant> declare_tool_changes(const std::vector<ai::MessageVariant>& transcript,
        std::vector<ai::MessageVariant> pending,
        const std::vector<ai::Tool>& executable_tools) {
    // pi takes the last system message in the pending list as the delta's
    // anchor; earlier system messages keep their declared tool fields.
    std::optional<std::size_t> anchor;
    for (std::size_t index = pending.size(); index > 0; --index) {
        if (std::holds_alternative<ai::SystemMessage>(pending[index - 1])) {
            anchor = index - 1;
            break;
        }
    }

    std::vector<ai::Tool> declared = current_tools(transcript);
    for (std::size_t index = 0; index < pending.size(); ++index) {
        const auto* system = std::get_if<ai::SystemMessage>(&pending[index]);
        if (system == nullptr) {
            continue;
        }
        if (anchor && index == *anchor) {
            // The anchor's tool fields are the delta's intent, not declared
            // state; the delta is recomputed from the committed transcript.
            continue;
        }
        apply_tool_delta(declared, *system);
    }

    std::vector<ai::Tool> target;
    target.reserve(executable_tools.size());
    for (const auto& tool : executable_tools) {
        target.push_back(tool_declaration(tool));
    }
    auto changes = tool_state_changes(declared, target);

    if (anchor) {
        auto& message = std::get<ai::SystemMessage>(pending[*anchor]);
        if (changes.empty() && message.tools_added.empty() && message.tools_removed.empty()) {
            return pending;
        }
        message.tools_added = std::move(changes.tools_added);
        message.tools_removed = std::move(changes.tools_removed);
        return pending;
    }
    if (changes.empty()) {
        return pending;
    }

    ai::SystemMessage declaration;
    declaration.timestamp = ai::current_timestamp_ms();
    declaration.tools_added = std::move(changes.tools_added);
    declaration.tools_removed = std::move(changes.tools_removed);
    const auto first_non_system = std::ranges::find_if(pending,
            [](const ai::MessageVariant& message) { return !std::holds_alternative<ai::SystemMessage>(message); });
    pending.insert(first_non_system, ai::MessageVariant{std::move(declaration)});
    return pending;
}

} // namespace cch::agent

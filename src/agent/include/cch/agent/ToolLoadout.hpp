#pragma once

#include <cch/ai/Message.hpp>
#include <cch/ai/Tool.hpp>

#include <vector>

namespace cch::agent {

/// pi `toToolDeclaration` (`packages/ai/src/utils/transcript.ts`): the
/// model-visible declaration of one tool. The executable field
/// (`outputSchema`) is dropped, so a transcript declares exactly the interface
/// the model is shown and a tool that only gained output metadata is not
/// re-declared.
[[nodiscard]] ai::Tool tool_declaration(const ai::Tool& tool);

/// pi `declarationsEqual`: whether two tools declare the same interface to the
/// model — name, description, parameters and `constrainedSampling`. Parameters
/// compare by their canonical JSON, because the C++ persistence layer reorders
/// object members while pi's JSON round trip keeps them stable; the canonical
/// form is the order-independent projection of pi's serialized comparison.
[[nodiscard]] bool declarations_equal(const ai::Tool& left, const ai::Tool& right);

/// pi `ToolStateChanges`: the ordered delta between two declared tool states.
/// A changed definition is a removal followed by an addition, so replaying the
/// delta always yields exactly the target state.
struct ToolStateChanges {
    /// Complete declarations that become available, in the target's order.
    std::vector<ai::Tool> tools_added{};
    /// Tools that stop being available, in the previous state's order.
    std::vector<ai::ToolReference> tools_removed{};

    [[nodiscard]] bool empty() const noexcept { return tools_added.empty() && tools_removed.empty(); }
};

/// pi `getToolStateChanges`: additions follow `current` order, removals follow
/// `previous` order.
[[nodiscard]] ToolStateChanges tool_state_changes(
        const std::vector<ai::Tool>& previous, const std::vector<ai::Tool>& current);

/// pi `getCurrentTools`: replay every system message's `toolsRemoved` and then
/// `toolsAdded` in transcript order. A redeclared name keeps the position of
/// its first declaration, which is the order the model has seen.
[[nodiscard]] std::vector<ai::Tool> current_tools(const std::vector<ai::MessageVariant>& messages);

/// pi `declareToolChanges` (`packages/agent/src/agent-loop.ts`): the pending
/// messages for the next request, with the delta between the transcript's
/// declared tool state and `executable_tools` applied.
///
/// The last system message in `pending` is the delta's anchor: its tool fields
/// are treated as intent and replaced, so a prompt-section patch carries the
/// tool delta on the same message. Without one, a non-empty delta becomes a new
/// system message before the first non-system pending message. The list comes
/// back unchanged when there is nothing to declare.
[[nodiscard]] std::vector<ai::MessageVariant> declare_tool_changes(const std::vector<ai::MessageVariant>& transcript,
        std::vector<ai::MessageVariant> pending,
        const std::vector<ai::Tool>& executable_tools);

} // namespace cch::agent

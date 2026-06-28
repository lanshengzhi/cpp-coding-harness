#pragma once

#include "CommandRegistry.hpp"
#include "PromptTemplateExpander.hpp"
#include "SkillExpander.hpp"

#include <string>
#include <string_view>

namespace cch::coding_agent {

/// Result of processing user input through the command/template/skill pipeline.
struct PromptProcessingResult {
    /// True if a slash-command consumed the input (no agent loop activation).
    bool command_handled{false};
    /// Message to display to the user when command_handled is true.
    std::optional<std::string> display_text;
    /// Text to pass to the agent loop (empty if command_handled).
    std::string expanded_prompt;
    /// True if the user requested shutdown (/quit).
    bool shutdown_requested{false};
};

/// Thin orchestrator that runs raw user input through skill expansion,
/// slash-command dispatch, and prompt-template expansion in order.
class PromptProcessingPipeline {
public:
    PromptProcessingPipeline(
        const PromptTemplateExpander& template_expander,
        CommandRegistry& command_registry,
        const SkillExpander& skill_expander);

    /// Process raw user input and return the result.
    [[nodiscard]] PromptProcessingResult process(
        std::string_view raw_input,
        const CommandContext& ctx) const;

private:
    const PromptTemplateExpander& template_expander_;
    CommandRegistry& command_registry_;
    const SkillExpander& skill_expander_;
};

} // namespace cch::coding_agent

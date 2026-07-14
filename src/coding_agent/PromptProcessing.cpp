#include "../../include/cch/coding_agent/PromptProcessing.hpp"

#include "coding_agent/prompt/PromptProcessor.hpp"

#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace cch::coding_agent {
namespace {

[[nodiscard]] PromptProcessingResult to_legacy_result(
    prompt::PromptProcessingOutcome outcome) {
    if (auto* agent_prompt = std::get_if<prompt::AgentPrompt>(&outcome)) {
        return PromptProcessingResult{
            .command_handled = false,
            .display_text = std::nullopt,
            .expanded_prompt = std::move(agent_prompt->text),
            .shutdown_requested = false,
        };
    }

    auto& handled = std::get<prompt::CommandHandled>(outcome);
    return PromptProcessingResult{
        .command_handled = true,
        .display_text = std::move(handled.feedback),
        .expanded_prompt = {},
        .shutdown_requested = handled.shutdown_requested,
    };
}

} // namespace

std::string expand_skill_command(
    std::string_view input,
    const std::vector<Skill>& skills,
    const harness::WorkspaceFileSystem& /*fs*/) {
    if (auto expanded = prompt::detail::try_expand_skill(input, skills)) {
        return std::move(*expanded);
    }
    return std::string{input};
}

SkillExpansionResult expand_skill_command_silent(
    std::string_view input,
    const std::vector<Skill>& skills) {
    return SkillExpansionResult{
        .expanded = prompt::detail::try_expand_skill(input, skills).value_or(std::string{input}),
        .diagnostics = {},
    };
}

PromptProcessingResult process_prompt(
    std::string_view raw_input,
    const std::vector<PromptTemplate>& templates,
    CommandRegistry& registry,
    const CommandContext& ctx) {
    static const std::vector<Skill> no_skills;
    return to_legacy_result(prompt::detail::process_prompt_borrowed(
        std::string{raw_input},
        registry,
        no_skills,
        templates,
        ctx));
}

PromptProcessingResult process_prompt(
    std::string_view raw_input,
    const std::vector<PromptTemplate>& templates,
    CommandRegistry& registry,
    const CommandContext& ctx,
    const std::vector<Skill>& skills,
    const harness::WorkspaceFileSystem& /*fs*/) {
    return to_legacy_result(prompt::detail::process_prompt_borrowed(
        std::string{raw_input},
        registry,
        skills,
        templates,
        ctx));
}

} // namespace cch::coding_agent

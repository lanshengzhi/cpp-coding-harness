#include "coding_agent/prompt/PromptProcessor.hpp"

#include "coding_agent/prompt/PromptTemplateExpander.hpp"
#include "coding_agent/SkillFormatting.hpp"

#include <algorithm>
#include <optional>
#include <string_view>
#include <utility>

namespace cch::coding_agent::prompt {
namespace {

[[nodiscard]] bool is_ascii_whitespace(char ch) {
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f' || ch == '\v';
}

struct SlashInvocation {
    std::string_view name;
    std::string_view arguments;
};

[[nodiscard]] std::optional<SlashInvocation> parse_slash_invocation(std::string_view input) {
    if (input.empty() || input.front() != '/') {
        return std::nullopt;
    }

    const auto body = input.substr(1);
    std::size_t delimiter = 0;
    while (delimiter < body.size() && !is_ascii_whitespace(body[delimiter])) {
        ++delimiter;
    }

    auto arguments = std::string_view{};
    if (delimiter < body.size()) {
        arguments = body.substr(delimiter + 1);
    }
    return SlashInvocation{body.substr(0, delimiter), arguments};
}

std::optional<std::string> try_expand_skill(
    std::string_view input,
    const std::vector<Skill>& skills) {
    if (!input.starts_with("/skill:")) {
        return std::nullopt;
    }

    const auto invocation = parse_slash_invocation(input);
    if (!invocation || !invocation->name.starts_with("skill:")) {
        return std::nullopt;
    }
    const auto skill_name = invocation->name.substr(6);
    if (skill_name.empty()) {
        return std::nullopt;
    }

    const auto found = std::find_if(
        skills.begin(),
        skills.end(),
        [skill_name](const Skill& skill) { return skill.name == skill_name; });
    if (found == skills.end()) {
        return std::nullopt;
    }

    return formatSkillInvocation(*found, found->content, invocation->arguments);
}

} // namespace

PromptProcessor::PromptProcessor(PromptResources resources)
    : commands_(std::move(resources.commands)),
      skills_(std::move(resources.skills)),
      templates_(std::move(resources.templates)) {}

PromptProcessingOutcome PromptProcessor::process(
    std::string input,
    CommandContext context) {
    const auto invocation = parse_slash_invocation(input);
    if (!invocation) {
        return AgentPrompt{std::move(input)};
    }

    context.available_commands = commands_.list_commands();
    try {
        if (auto handled = commands_.dispatch(invocation->name, context, invocation->arguments)) {
            const bool shutdown = handled->shutdown_requested;
            return CommandHandled{
                .code = shutdown ? "shutdown" : "command_handled",
                .feedback = std::move(handled->display_text),
                .shutdown_requested = shutdown,
            };
        }
    } catch (...) {
        return CommandHandled{
            .code = "command_handler_failed",
            .feedback = "Command handler failed.",
            .shutdown_requested = false,
        };
    }

    std::string expanded = std::move(input);
    if (auto expanded_skill = try_expand_skill(expanded, skills_)) {
        expanded = std::move(*expanded_skill);
    }
    if (auto expanded_template = try_expand_prompt_template(expanded, templates_)) {
        expanded = std::move(*expanded_template);
    }

    return AgentPrompt{std::move(expanded)};
}

} // namespace cch::coding_agent::prompt

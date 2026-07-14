#include "coding_agent/prompt/PromptProcessor.hpp"

#include "coding_agent/prompt/PromptTemplateExpander.hpp"
#include "coding_agent/prompt/SlashCommandParser.hpp"
#include "coding_agent/SkillFormatting.hpp"

#include <algorithm>
#include <optional>
#include <string_view>
#include <utility>

namespace cch::coding_agent::prompt {
namespace {

std::optional<std::string> try_expand_skill(
    std::string_view input,
    const std::vector<Skill>& skills) {
    if (!input.starts_with("/skill:")) {
        return std::nullopt;
    }

    const auto invocation = prompt::try_parse_slash_command(input);
    if (!invocation || !invocation->first.starts_with("skill:")) {
        return std::nullopt;
    }
    const auto skill_name = invocation->first.substr(6);
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

    return formatSkillInvocation(*found, found->content, invocation->second);
}

} // namespace

PromptProcessor::PromptProcessor(std::vector<Skill> skills, std::vector<PromptTemplate> templates)
    : skills_(std::move(skills)),
      templates_(std::move(templates)) {}

AgentPrompt PromptProcessor::process(
    std::string input,
    bool expand_templates) {
    if (!expand_templates) {
        return AgentPrompt{std::move(input)};
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

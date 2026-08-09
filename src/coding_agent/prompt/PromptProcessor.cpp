#include "coding_agent/prompt/PromptProcessor.hpp"

#include "coding_agent/prompt/PromptTemplateExpander.hpp"
#include "coding_agent/SkillFormatting.hpp"

#include <string_view>
#include <utility>

namespace cch::coding_agent::prompt {

PromptProcessor::PromptProcessor(std::vector<Skill> skills, std::vector<PromptTemplate> templates)
    : skills_(std::move(skills)),
      templates_(std::move(templates)) {}

AgentPrompt PromptProcessor::process(
    std::string input,
    bool expand_templates) {
    if (!expand_templates) {
        return AgentPrompt{std::move(input)};
    }

    // pi prompt expansion order: the skill command expands first (reading
    // the skill file at invocation time), then prompt templates.
    std::string expanded = expandSkillCommand(input, skills_);
    if (auto expanded_template = try_expand_prompt_template(expanded, templates_)) {
        expanded = std::move(*expanded_template);
    }

    return AgentPrompt{std::move(expanded)};
}

} // namespace cch::coding_agent::prompt

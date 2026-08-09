#include "coding_agent/prompt/PromptExpansion.hpp"

#include "coding_agent/SkillFormatting.hpp"
#include "coding_agent/prompt/PromptTemplateExpander.hpp"

#include <string>
#include <utility>

namespace cch::coding_agent::prompt {

std::string expand_prompt_input(
    std::string text,
    const std::vector<Skill>& skills,
    const std::vector<PromptTemplate>& templates,
    bool expand_prompt_templates) {
    if (!expand_prompt_templates) {
        return text;
    }
    std::string expanded = expandSkillCommand(std::move(text), skills);
    if (auto expanded_template =
            try_expand_prompt_template(expanded, templates)) {
        expanded = std::move(*expanded_template);
    }
    return expanded;
}

} // namespace cch::coding_agent::prompt

#include "../../include/cch/coding_agent/PromptTemplate.hpp"
#include "../../include/cch/coding_agent/PromptTemplateExpander.hpp"

#include <string_view>
#include <vector>

namespace cch::coding_agent {

// Forward to the existing pure expansion function in PromptExpander.cpp.
std::string expand_prompt_template(std::string_view input, const std::vector<PromptTemplate>& templates);

PromptTemplateExpander::PromptTemplateExpander(const std::vector<PromptTemplate>& templates)
    : templates_(templates) {}

std::string PromptTemplateExpander::expand(std::string_view input) const {
    return expand_prompt_template(input, templates_);
}

} // namespace cch::coding_agent

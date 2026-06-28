#pragma once

#include "PromptTemplate.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace cch::coding_agent {

/// Expands `/templateName args` input using bash-style argument substitution.
class PromptTemplateExpander {
public:
    explicit PromptTemplateExpander(const std::vector<PromptTemplate>& templates);

    /// Returns expanded text, or the original input if no template matched.
    [[nodiscard]] std::string expand(std::string_view input) const;

private:
    const std::vector<PromptTemplate>& templates_;
};

} // namespace cch::coding_agent

#pragma once

#include <cch/coding_agent/PromptTemplate.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cch::coding_agent::prompt {

/// Expand a matching prompt template from the session's immutable snapshot.
/// Returns std::nullopt when input is not a matching template invocation.
[[nodiscard]] std::optional<std::string> try_expand_prompt_template(
    std::string_view input,
    const std::vector<PromptTemplate>& templates);

} // namespace cch::coding_agent::prompt

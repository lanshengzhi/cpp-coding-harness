#pragma once

#include <optional>
#include <string>

namespace cch::coding_agent {

/// Passive-value prompt template definition.
struct PromptTemplate {
    std::string name;
    std::optional<std::string> description;
    std::string content;
    /// Optional argument hint for autocomplete display (stored for future TUI use).
    std::optional<std::string> argument_hint = std::nullopt;
};

} // namespace cch::coding_agent

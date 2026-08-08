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
    /// Absolute path to the template file (pi `PromptTemplate.filePath`),
    /// used for collision diagnostics and source presentation.
    std::string filePath;
};

} // namespace cch::coding_agent

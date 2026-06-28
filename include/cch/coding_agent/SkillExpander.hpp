#pragma once

#include "Skill.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace cch::harness {
class WorkspaceFileSystem;
}

namespace cch::coding_agent {

/// Result of silent skill expansion (no stderr output).
struct SkillExpansionResult {
    /// Expanded prompt text, or the original input if no skill matched.
    std::string expanded;
    /// Diagnostics produced during expansion (e.g. unknown skill name).
    std::vector<std::string> diagnostics;
};

/// Expands `/skill:name` commands into the full skill instructions block.
class SkillExpander {
public:
    SkillExpander(const std::vector<Skill>& skills, const harness::WorkspaceFileSystem& fs);

    /// Expand a skill command silently; diagnostics are returned as values.
    [[nodiscard]] SkillExpansionResult expand(std::string_view input) const;

    /// Expand a skill command and print diagnostics to stderr.
    [[nodiscard]] std::string expand_and_print(std::string_view input) const;

private:
    const std::vector<Skill>& skills_;
    const harness::WorkspaceFileSystem& fs_;
};

} // namespace cch::coding_agent

#include "../../include/cch/coding_agent/Skill.hpp"
#include "../../include/cch/coding_agent/SkillExpander.hpp"
#include "coding_agent/SkillFormatting.hpp"
#include "harness/WorkspaceFileSystem.hpp"

#include <iostream>
#include <string_view>

namespace cch::coding_agent {
namespace {

[[nodiscard]] std::string_view trim_left(std::string_view sv) {
    while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t')) {
        sv.remove_prefix(1);
    }
    return sv;
}

} // namespace

SkillExpander::SkillExpander(const std::vector<Skill>& skills, const harness::WorkspaceFileSystem& fs)
    : skills_(skills), fs_(fs) {}

SkillExpansionResult SkillExpander::expand(std::string_view input) const {
    // Fast path: not a skill command
    if (!input.starts_with("/skill:")) {
        return {std::string{input}, {}};
    }

    // Parse skill name: text between /skill: and first space (or end of string)
    std::string_view rest = input.substr(7); // strip "/skill:"
    auto space_pos = rest.find_first_of(" \t");
    std::string_view name = (space_pos == std::string_view::npos)
        ? rest
        : rest.substr(0, space_pos);

    // Bare /skill: with no name — passthrough
    if (name.empty()) {
        return {std::string{input}, {}};
    }

    // Parse args: everything after the skill name
    std::string_view args = (space_pos == std::string_view::npos)
        ? std::string_view{}
        : trim_left(rest.substr(space_pos + 1));

    // Look up skill by name
    const Skill* found = nullptr;
    for (const auto& skill : skills_) {
        if (skill.name == name) {
            found = &skill;
            break;
        }
    }

    if (!found) {
        std::vector<std::string> diags;
        diags.push_back(std::string{"[skill:warn] unknown skill: "} + std::string{name});
        return {std::string{input}, std::move(diags)};
    }

    // Use the cached skill content (already body after frontmatter from loader).
    return {formatSkillInvocation(*found, found->content, args), {}};
}

std::string SkillExpander::expand_and_print(std::string_view input) const {
    auto result = expand(input);
    for (const auto& diag : result.diagnostics) {
        std::cerr << diag << '\n';
    }
    return std::move(result.expanded);
}

} // namespace cch::coding_agent

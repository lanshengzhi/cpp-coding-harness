#pragma once

#include <cch/coding_agent/Skill.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace cch::coding_agent {

/// Format visible skills (disableModelInvocation == false) as an
/// <available_skills> XML block for system prompt / agent context injection.
///
/// Follows the Agent Skills standard format used by pi:
/// prose intro paragraph, wrapped XML, one <skill> per entry with
/// <name>, <description>, <location> children.
///
/// Returns an empty string when no visible skills remain.
[[nodiscard]] std::string formatSkillsForPrompt(const std::vector<Skill>& skills);

/// Format a single skill invocation as a <skill> XML block for
/// /skill:name command expansion.
///
/// The skill content (body after YAML frontmatter) is placed inside the
/// <skill> element. References-are-relative-to preamble uses the parent
/// directory of the skill file.
///
/// When additional_instructions is non-empty, it is appended after the
/// closing </skill> tag separated by a blank line.
[[nodiscard]] std::string formatSkillInvocation(
    const Skill& skill,
    const std::string& content,
    std::string_view additional_instructions = {});

} // namespace cch::coding_agent

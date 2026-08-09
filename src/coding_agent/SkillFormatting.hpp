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
/// /skill:name command expansion (pi `agent-session.ts` `_expandSkillCommand`).
///
/// The skill content (body after YAML frontmatter, read at invocation time)
/// is placed inside the <skill> element. The references-are-relative-to
/// preamble uses the skill's own base directory (pi `Skill.baseDir`).
///
/// When additional_instructions is non-empty, it is appended after the
/// closing </skill> tag separated by a blank line.
[[nodiscard]] std::string formatSkillInvocation(
    const Skill& skill,
    std::string_view content,
    std::string_view additional_instructions = {});

/// pi `_expandSkillCommand` (`core/agent-session.ts`): expand a
/// `/skill:name [args]` input at invocation time by reading the skill file
/// from disk. The body (after YAML frontmatter) is trimmed and wrapped in
/// pi's block shape; non-empty args are appended after a blank line. Unknown
/// skill names, malformed invocations, and read failures pass the input
/// through unchanged (pi returns the original text on read failure).
[[nodiscard]] std::string expandSkillCommand(
    std::string_view text,
    const std::vector<Skill>& skills);

} // namespace cch::coding_agent

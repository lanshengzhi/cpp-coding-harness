#pragma once

#include <string>
#include <vector>

namespace cch::coding_agent {

/// Stable diagnostic code for skill loading issues.
enum class SkillDiagnosticCode {
    file_info_failed,
    list_failed,
    read_failed,
    parse_failed,
    invalid_metadata,
    duplicate_name,
};

/// Warning produced while loading skills. Failures are warnings, not errors —
/// invalid or unreadable skills are skipped rather than aborting startup.
struct SkillDiagnostic {
    /// Diagnostic severity. Currently only "warning" is emitted.
    std::string type{"warning"};
    /// Stable diagnostic code.
    SkillDiagnosticCode code{SkillDiagnosticCode::invalid_metadata};
    /// Human-readable diagnostic message.
    std::string message;
    /// Path associated with the diagnostic.
    std::string path;
};

/// Skill loaded from a SKILL.md file.
///
/// name, description, and filePath are inserted into the system prompt in an
/// XML-formatted block per the Agent Skills standard.
struct Skill {
    /// Stable skill name used for lookup and model-visible listings.
    std::string name;
    /// Short model-visible description of when to use the skill.
    std::string description;
    /// Full skill instructions (body after YAML frontmatter).
    std::string content;
    /// Absolute path to the skill file. Used for model-visible location and
    /// resolving relative references.
    std::string filePath;
    /// Exclude this skill from model-visible skill lists while still allowing
    /// explicit application invocation.
    bool disableModelInvocation{false};
};

/// Result of a skill loading operation — returned by both single-file and
/// directory-discovery loading functions.
struct SkillLoadResult {
    /// Successfully loaded skills.
    std::vector<Skill> skills;
    /// Diagnostics collected during loading (warnings, not errors).
    std::vector<SkillDiagnostic> diagnostics;
};

} // namespace cch::coding_agent

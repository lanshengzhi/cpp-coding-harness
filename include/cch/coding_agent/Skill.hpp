#pragma once

#include <cch/coding_agent/ProjectResources.hpp>

#include <optional>
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
    collision,
};

/// Diagnostic produced while loading skills, in pi's `ResourceDiagnostic`
/// shape (`core/skills.ts`): failures are warnings, not errors — invalid or
/// unreadable skills are skipped rather than aborting startup; duplicate
/// names emit the collision kind with winner/loser paths.
struct SkillDiagnostic {
    /// pi `ResourceDiagnostic.type` — "warning" | "collision".
    std::string type{"warning"};
    /// Stable diagnostic code.
    SkillDiagnosticCode code{SkillDiagnosticCode::invalid_metadata};
    /// Human-readable diagnostic message.
    std::string message;
    /// Path associated with the diagnostic.
    std::string path;
    /// pi `ResourceDiagnostic.collision` payload for duplicate-name
    /// collisions (winner/loser paths).
    std::optional<ResourceCollision> collision;
};

/// Skill loaded from a SKILL.md file.
///
/// name, description, and filePath are inserted into the system prompt in an
/// XML-formatted block per the Agent Skills standard. The file body is NOT
/// preloaded: `/skill:name` invocation reads the file at invocation time
/// (pi `agent-session.ts` `_expandSkillCommand`), so the value stays a
/// passive discovery record.
struct Skill {
    /// Stable skill name used for lookup and model-visible listings.
    std::string name;
    /// Short model-visible description of when to use the skill.
    std::string description;
    /// Absolute path to the skill file. Used for model-visible location and
    /// resolving relative references.
    std::string filePath;
    /// The skill directory (parent of SKILL.md / dirname of the path): the
    /// base against which the file's relative references resolve (pi
    /// `Skill.baseDir`).
    std::string baseDir;
    /// pi `Skill.sourceInfo`: the discovery provenance (scope, source, and
    /// the resource-root baseDir recorded by the loader).
    SourceInfo sourceInfo;
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

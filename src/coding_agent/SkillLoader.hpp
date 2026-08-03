#pragma once

#include <cch/coding_agent/Skill.hpp>

#include <string>

namespace cch::harness {
class WorkspaceFileSystem;
}

namespace cch::coding_agent {

/// A single skill directory specification for skill discovery.
struct SkillDirSpec {
    /// Absolute or workspace-relative path to scan.
    std::string path;
    /// When true, root-level .md files in this directory are loaded as skills
    /// (used for global ~/.pi/agent/skills/). When false, only nested
    /// SKILL.md files within subdirectories are discovered (project dirs).
    bool includeRootFiles{false};
};

/// Load a single SKILL.md file from the given absolute path.
///
/// Reads the file via the filesystem, parses YAML frontmatter, validates
/// skill metadata, and returns the loaded Skill with any diagnostics.
/// Returns only diagnostics (no Skill) when validation fails fatally
/// (missing/empty description) or when the file cannot be read/parsed.
[[nodiscard]] SkillLoadResult loadSkillFromFile(
    const harness::WorkspaceFileSystem& fs,
    const std::string& filePath);

/// Discover and load skills from one or more directories.
///
/// Each SkillDirSpec specifies a directory to scan and whether root .md
/// files should be treated as skills. The walk is recursive: each directory
/// is scanned for SKILL.md files (one per directory), dot-prefixed entries
/// and node_modules are skipped, and symlinks are resolved.
/// Duplicate skill names drop subsequent occurrences with a diagnostic.
[[nodiscard]] SkillLoadResult loadSkills(
    const harness::WorkspaceFileSystem& fs,
    const std::vector<SkillDirSpec>& dirs);

} // namespace cch::coding_agent

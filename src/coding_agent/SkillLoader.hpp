#pragma once

#include <cch/coding_agent/ProjectResources.hpp>
#include <cch/coding_agent/Skill.hpp>
#include <cch/agent/harness/FileSystem.hpp>

#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace cch::harness {
class WorkspaceFileSystem;
}

namespace cch::coding_agent {

/// pi `PathMetadata` subset carried by one skill scan: the scope and
/// resource-root baseDir recorded into each discovered skill's `sourceInfo`
/// (pi `resource-loader.ts` `updateSkillsFromPaths` final shape: `source`
/// "auto" for discovery, "cli" for explicit `--skill` paths).
struct SkillSourceContext {
    /// pi `SourceInfo.source` — "auto" (discovered) or "cli" (explicit path).
    std::string source{"auto"};
    SourceScope scope{SourceScope::Temporary};
    /// pi `PathMetadata.baseDir` — the resource root (agent dir, project
    /// `.pi` dir, or the owning `.agents` directory); absent for explicit
    /// CLI paths.
    std::optional<std::string> base_dir;
};

/// A single skill directory specification for skill discovery.
struct SkillDirSpec {
    /// Path relative to the filesystem root the scan is addressed against
    /// (workspace-relative, or relative to the user/`.agents` directory
    /// filesystem instance created for the scan).
    std::string path;
    /// When true, root-level .md files in this directory are loaded as skills
    /// (pi "pi" discovery mode: user `~/.pi/agent/skills`, project `.pi/skills`,
    /// and explicit `--skill` paths). When false, only nested SKILL.md files
    /// within subdirectories are discovered (pi "agents" mode for the
    /// `.agents/skills` convention).
    bool include_root_files{false};
    /// The scope and resource-root baseDir recorded into each discovered
    /// skill's `sourceInfo`.
    SkillSourceContext source_context{};
};

/// Load a single SKILL.md file through the canonical asynchronous filesystem
/// capability.
[[nodiscard]] support::AsyncResult<SkillLoadResult, harness::FileError> loadSkillFromFile(harness::AsyncFileSystem& fs,
        std::string file_path,
        SkillSourceContext source_context = {},
        std::stop_token stop_token = {});

/// Temporary expand-contract bridge for synchronous callers. New production
/// resource loading uses the AsyncFileSystem overload above.
[[nodiscard]] SkillLoadResult loadSkillFromFile(
    const harness::WorkspaceFileSystem& fs,
    const std::string& filePath,
    SkillSourceContext source_context = {});

/// Discover and load skills from one or more directories.
///
/// Each SkillDirSpec specifies a directory to scan and whether root .md
/// files should be treated as skills (pi "pi" mode). The walk is recursive
/// with pi's `.gitignore`/`.ignore`/`.fdignore` matcher (prefix/negation
/// semantics): each scanned directory is checked for SKILL.md files (one per
/// directory), dot-prefixed entries and node_modules are skipped, symlinks
/// are resolved, and the same real file reached twice (via symlink) is
/// silently deduplicated. Duplicate skill names drop subsequent occurrences
/// with a pi-shaped collision diagnostic.
[[nodiscard]] support::AsyncResult<SkillLoadResult, harness::FileError> loadSkills(
        harness::AsyncFileSystem& fs, std::vector<SkillDirSpec> dirs, std::stop_token stop_token = {});

/// Temporary expand-contract bridge for synchronous callers.
[[nodiscard]] SkillLoadResult loadSkills(
    const harness::WorkspaceFileSystem& fs,
    const std::vector<SkillDirSpec>& dirs);

} // namespace cch::coding_agent

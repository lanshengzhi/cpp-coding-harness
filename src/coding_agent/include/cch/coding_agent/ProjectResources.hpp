#pragma once

#include <cch/agent/harness/FileSystem.hpp>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <stop_token>
#include <vector>

namespace cch::coding_agent {

/// One explicitly authorized path capability supplied by coding-agent
/// composition. Resource loading may select only a capability from this
/// collection; it never creates a filesystem for a path it discovers.
struct AuthorizedResourcePath {
    std::string path;
    std::shared_ptr<harness::AsyncFileSystem> filesystem;
};

/// Filesystem capabilities authorized by Session composition for project
/// resource discovery. The vectors contain already-opened capabilities for
/// the workspace/ancestor and git roots in discovery order. The loader has no
/// arbitrary-root factory and cannot widen this set.
struct ProjectResourceFileSystems {
    std::shared_ptr<harness::AsyncFileSystem> workspace;
    std::vector<std::shared_ptr<harness::AsyncFileSystem>> ancestor_roots;
    std::vector<std::shared_ptr<harness::AsyncFileSystem>> git_roots;
    std::shared_ptr<harness::AsyncFileSystem> agent_config_directory;
    std::shared_ptr<harness::AsyncFileSystem> user_agents_root;
    std::vector<AuthorizedResourcePath> explicit_paths;
};

/// pi `SourceScope` subset (`core/source-info.ts`): the scope recorded on a
/// resource's `SourceInfo`. `user` and `project` group the loaded-resources
/// presentation; `temporary` is the CLI/explicit-path scope (pi's
/// "path" group).
enum class SourceScope {
    User,
    Project,
    Temporary,
};

/// pi `SourceOrigin` subset (`core/source-info.ts`): where a resource path
/// came from. Only `TopLevel` is produced in the loader subset (the
/// package-manager `package` origin is outside it, with no extensions/
/// package-manager surface).
enum class SourceOrigin {
    TopLevel,
    Package,
};

/// pi `SourceInfo` subset (`core/source-info.ts`): the provenance recorded
/// on each loaded resource (skills, prompt templates, themes). The loader
/// subset produces the pi resource-loader final shape: `source` "auto" for
/// discovered resources or "cli" for explicit CLI paths, the resource
/// scope, and the resource-root `baseDir` (agent dir, project `.pi` dir, or
/// the owning `.agents` directory).
struct SourceInfo {
    /// pi `SourceInfo.path` — the resource file path.
    std::string path;
    /// pi `SourceInfo.source` — "auto" (discovered) or "cli" (explicit
    /// `--skill`/`--prompt-template`/`--theme` path).
    std::string source{"auto"};
    SourceScope scope{SourceScope::Temporary};
    SourceOrigin origin{SourceOrigin::TopLevel};
    /// pi `SourceInfo.baseDir?` — the resource root the path was discovered
    /// under; absent for explicit CLI paths.
    std::optional<std::string> base_dir;
};

/// pi `ResourceCollision` subset (`core/diagnostics.ts`): the collision
/// payload of a pi `ResourceDiagnostic`. The `extension` resource type is
/// outside the loader subset (no extensions/package-manager surface).
enum class ResourceCollisionResourceType {
    Skill,
    Prompt,
    Theme,
};

struct ResourceCollision {
    ResourceCollisionResourceType resource_type{ResourceCollisionResourceType::Skill};
    /// Skill name, prompt name, or theme name.
    std::string name;
    /// Path of the resource that won the name (loaded first).
    std::string winner_path;
    /// Path of the resource that lost the name (skipped).
    std::string loser_path;
    /// Optional source identifiers ("local", "user", "project", "cli").
    std::optional<std::string> winner_source;
    std::optional<std::string> loser_source;
};

/// pi `ResourceDiagnostic` shape (`core/diagnostics.ts`): type/message/path,
/// with an optional collision payload carrying the winner/loser paths.
enum class ResourceDiagnosticType {
    Warning,
    Error,
    Collision,
};

struct ResourceDiagnostic {
    ResourceDiagnosticType type{ResourceDiagnosticType::Warning};
    std::string message;
    /// Associated filesystem path, when any (pi `ResourceDiagnostic.path?`).
    std::optional<std::string> path;
    /// Present only for `Collision` diagnostics.
    std::optional<ResourceCollision> collision;
};

/// One theme document collected by the resource loader — pi
/// `resource-loader.ts` `loadThemes` reads every source and hands the parsed
/// themes to `setRegisteredThemes`. Parsing and name-level dedupe stay in
/// the physically separate coding-agent TUI package (`discover_themes`), so
/// the loader carries documents (source path + raw JSON), not parsed
/// themes.
struct LoadedThemeResource {
    /// Display/source path (pi `Theme.sourcePath`): `.pi/themes/<name>` for
    /// project themes, the absolute user-directory path, or the resolved
    /// explicit `--theme` path.
    std::string path;
    std::string json;
    /// pi `SourceInfo.scope` of the discovering source: `Project` (the
    /// trust-gated `.pi/themes` directory), `User` (`~/.pi/agent/themes`),
    /// or `Temporary` (an explicit `--theme` path).
    SourceScope scope{SourceScope::Project};
};

/// Trust-requiring project resource markers: the `.pi/` markers (the pi
/// `TRUST_REQUIRING_PROJECT_CONFIG_RESOURCES` subset without the extensions /
/// package-manager markers and with `settings.json` owned by the Settings
/// Manager, "trusted means load", #327) plus the `.agents/skills` convention
/// directories in the workspace's ancestor chain (pi `trust-manager.ts`
/// `hasTrustRequiringProjectResources`). A loadable marker's mere presence
/// triggers the boot Project Trust decision.
enum class ProjectResourceKind {
    ProjectSkills,
    ProjectPrompts,
    ProjectThemes,
    ProjectSystemPrompt,
    ProjectAppendSystemPrompt,
    /// An `.agents/skills` directory in the workspace or an ancestor up to
    /// the filesystem root, excluding the user's `~/.agents/skills`.
    ProjectAgentsSkills,
};

struct DetectedProjectResource {
    ProjectResourceKind kind{ProjectResourceKind::ProjectSkills};
    std::string path;
    bool loadable{true};
};

struct ProjectResourceDetectionResult {
    std::vector<DetectedProjectResource> resources;
    std::vector<ResourceDiagnostic> diagnostics;
};

[[nodiscard]] std::string_view to_string(ProjectResourceKind kind);

[[nodiscard]] support::AsyncResult<ProjectResourceDetectionResult, harness::FileError> detect_project_resources(
        ProjectResourceFileSystems filesystems,
        const std::filesystem::path& user_agents_skills_dir,
        std::stop_token stop_token = {});

[[nodiscard]] bool has_detected_kind(
    const ProjectResourceDetectionResult& detection,
    ProjectResourceKind kind);

/// True when any loadable trust-requiring marker is present. The trust
/// decision then gates all project resource loading: trusted means load,
/// untrusted means none of the project resources load.
[[nodiscard]] bool needs_project_trust_resolution(
    const ProjectResourceDetectionResult& detection);

} // namespace cch::coding_agent

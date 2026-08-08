#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cch::harness {
class WorkspaceFileSystem;
}

namespace cch::coding_agent {

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

/// Trust-requiring project resource markers under `.pi/` — the pi
/// `TRUST_REQUIRING_PROJECT_CONFIG_RESOURCES` subset without the extensions /
/// package-manager markers and with `settings.json` owned by the Settings
/// Manager ("trusted means load", #327). A loadable marker's mere presence
/// triggers the boot Project Trust decision.
enum class ProjectResourceKind {
    ProjectSkills,
    ProjectPrompts,
    ProjectThemes,
    ProjectSystemPrompt,
    ProjectAppendSystemPrompt,
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

[[nodiscard]] ProjectResourceDetectionResult detect_project_resources(
    const harness::WorkspaceFileSystem& fs);

[[nodiscard]] bool has_detected_kind(
    const ProjectResourceDetectionResult& detection,
    ProjectResourceKind kind);

/// True when any loadable trust-requiring marker is present. The trust
/// decision then gates all project resource loading: trusted means load,
/// untrusted means none of the project resources load.
[[nodiscard]] bool needs_project_trust_resolution(
    const ProjectResourceDetectionResult& detection);

} // namespace cch::coding_agent

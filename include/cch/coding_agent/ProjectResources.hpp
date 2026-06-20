#pragma once

#include <cch/coding_agent/ProjectTrust.hpp>

#include <optional>
#include <string>
#include <vector>

namespace cch::harness {
class WorkspaceFileSystem;
}

namespace cch::coding_agent {

enum class ProjectResourceKind {
    ProjectSettings,
    ProjectSkills,
    ProjectPrompts,
    ProjectExtensions,
    ProjectPackages,
    ProjectSystemPrompt,
    ProjectAppendSystemPrompt,
};

enum class ResourceEnablement {
    Auto,
    On,
    Off,
};

enum class ResourceDiagnosticSeverity {
    Info,
    Warning,
    Error,
};

enum class ResourceSkipReason {
    NotDetected,
    Allowed,
    Disabled,
    Untrusted,
    Unsupported,
    DetectionError,
};

struct ResourceDiagnostic {
    ResourceDiagnosticSeverity severity{ResourceDiagnosticSeverity::Warning};
    std::string code;
    std::string message;
    std::string path;
    std::optional<ProjectResourceKind> kind;
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

struct ResourceLoadDecision {
    ProjectResourceKind kind{ProjectResourceKind::ProjectSkills};
    bool detected{false};
    bool allowed{false};
    ResourceSkipReason reason{ResourceSkipReason::NotDetected};
    std::string path;
    std::string message;
};

struct ProjectResourcePolicy {
    ResourceEnablement project_skills{ResourceEnablement::Auto};
};

struct ProjectResourceLoadPlan {
    std::vector<ResourceLoadDecision> decisions;
    std::vector<ResourceDiagnostic> diagnostics;
    bool project_trust_required{false};
    bool skipped_for_untrusted{false};
};

[[nodiscard]] std::string to_string(ProjectResourceKind kind);
[[nodiscard]] std::string to_string(ResourceEnablement enablement);
[[nodiscard]] std::string to_string(ResourceSkipReason reason);

[[nodiscard]] std::optional<ResourceEnablement> parse_resource_enablement(const std::string& value);

[[nodiscard]] ProjectResourceDetectionResult detect_project_resources(
    const harness::WorkspaceFileSystem& fs);

[[nodiscard]] bool has_detected_kind(
    const ProjectResourceDetectionResult& detection,
    ProjectResourceKind kind);

[[nodiscard]] bool needs_project_trust_resolution(
    const ProjectResourceDetectionResult& detection,
    const ProjectResourcePolicy& policy);

[[nodiscard]] ProjectResourceLoadPlan build_project_resource_load_plan(
    const ProjectResourceDetectionResult& detection,
    const ProjectResourcePolicy& policy,
    const ProjectTrustResolution& trust);

[[nodiscard]] bool project_skills_allowed(const ProjectResourceLoadPlan& plan);

} // namespace cch::coding_agent

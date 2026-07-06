#include "../../include/cch/coding_agent/ProjectResources.hpp"

#include "../harness/WorkspaceFileSystem.hpp"

#include <array>
#include <filesystem>
#include <optional>

namespace cch::coding_agent {
namespace {

struct MarkerSpec {
    ProjectResourceKind kind;
    const char* path;
    harness::FileKind expected_kind;
};

constexpr std::array<MarkerSpec, 7> kMarkers{{
    {ProjectResourceKind::ProjectSettings, ".cpp-harness/settings.json", harness::FileKind::File},
    {ProjectResourceKind::ProjectSkills, ".cpp-harness/skills", harness::FileKind::Directory},
    {ProjectResourceKind::ProjectPrompts, ".cpp-harness/prompts", harness::FileKind::Directory},
    {ProjectResourceKind::ProjectExtensions, ".cpp-harness/extensions", harness::FileKind::Directory},
    {ProjectResourceKind::ProjectPackages, ".cpp-harness/packages", harness::FileKind::Directory},
    {ProjectResourceKind::ProjectSystemPrompt, ".cpp-harness/SYSTEM.md", harness::FileKind::File},
    {ProjectResourceKind::ProjectAppendSystemPrompt, ".cpp-harness/APPEND_SYSTEM.md", harness::FileKind::File},
}};

[[nodiscard]] bool kind_matches(harness::FileKind actual, harness::FileKind expected) {
    return actual == expected;
}

[[nodiscard]] harness::FileKind canonical_kind(const std::string& path) {
    std::error_code ec;
    auto status = std::filesystem::status(path, ec);
    if (ec) {
        return harness::FileKind::File;
    }
    if (std::filesystem::is_directory(status)) {
        return harness::FileKind::Directory;
    }
    if (std::filesystem::is_symlink(status)) {
        return harness::FileKind::Symlink;
    }
    return harness::FileKind::File;
}

[[nodiscard]] ResourceDiagnostic diagnostic(
    ResourceDiagnosticSeverity severity,
    std::string code,
    std::string message,
    std::string path,
    ProjectResourceKind kind) {
    return ResourceDiagnostic{
        .severity = severity,
        .code = std::move(code),
        .message = std::move(message),
        .path = std::move(path),
        .kind = kind,
    };
}

[[nodiscard]] ResourceEnablement enablement_for(
    const ProjectResourcePolicy& policy,
    ProjectResourceKind kind) {
    switch (kind) {
    case ProjectResourceKind::ProjectSkills:
        return policy.project_skills;
    case ProjectResourceKind::ProjectPrompts:
        if (policy.project_skills == ResourceEnablement::Off ||
            policy.project_prompts == ResourceEnablement::Off) {
            return ResourceEnablement::Off;
        }
        if (policy.project_prompts == ResourceEnablement::On) {
            return ResourceEnablement::On;
        }
        return policy.project_skills;
    case ProjectResourceKind::ProjectSettings:
    case ProjectResourceKind::ProjectExtensions:
    case ProjectResourceKind::ProjectPackages:
    case ProjectResourceKind::ProjectSystemPrompt:
    case ProjectResourceKind::ProjectAppendSystemPrompt:
        return ResourceEnablement::Auto;
    }
    return ResourceEnablement::Auto;
}

[[nodiscard]] bool has_implemented_loader(ProjectResourceKind kind) {
    return kind == ProjectResourceKind::ProjectSkills ||
           kind == ProjectResourceKind::ProjectPrompts;
}

} // namespace

std::string to_string(ProjectResourceKind kind) {
    switch (kind) {
    case ProjectResourceKind::ProjectSettings:
        return "project_settings";
    case ProjectResourceKind::ProjectSkills:
        return "project_skills";
    case ProjectResourceKind::ProjectPrompts:
        return "project_prompts";
    case ProjectResourceKind::ProjectExtensions:
        return "project_extensions";
    case ProjectResourceKind::ProjectPackages:
        return "project_packages";
    case ProjectResourceKind::ProjectSystemPrompt:
        return "project_system_prompt";
    case ProjectResourceKind::ProjectAppendSystemPrompt:
        return "project_append_system_prompt";
    }
    return "project_resource";
}

std::string to_string(ResourceEnablement enablement) {
    switch (enablement) {
    case ResourceEnablement::Auto:
        return "auto";
    case ResourceEnablement::On:
        return "on";
    case ResourceEnablement::Off:
        return "off";
    }
    return "auto";
}

std::string to_string(ResourceSkipReason reason) {
    switch (reason) {
    case ResourceSkipReason::NotDetected:
        return "not_detected";
    case ResourceSkipReason::Allowed:
        return "allowed";
    case ResourceSkipReason::Disabled:
        return "disabled";
    case ResourceSkipReason::Untrusted:
        return "untrusted";
    case ResourceSkipReason::Unsupported:
        return "unsupported";
    case ResourceSkipReason::DetectionError:
        return "detection_error";
    }
    return "unknown";
}

std::optional<ResourceEnablement> parse_resource_enablement(const std::string& value) {
    if (value == "auto") {
        return ResourceEnablement::Auto;
    }
    if (value == "on") {
        return ResourceEnablement::On;
    }
    if (value == "off") {
        return ResourceEnablement::Off;
    }
    return std::nullopt;
}

ProjectResourceDetectionResult detect_project_resources(const harness::WorkspaceFileSystem& fs) {
    ProjectResourceDetectionResult result;

    for (const auto& marker : kMarkers) {
        auto info = fs.fileInfo(marker.path);
        if (!info) {
            if (info.error().code == harness::FileErrorCode::NotFound) {
                continue;
            }
            result.diagnostics.push_back(diagnostic(
                ResourceDiagnosticSeverity::Warning,
                "marker_info_failed",
                info.error().message,
                marker.path,
                marker.kind));
            continue;
        }

        bool loadable = true;
        if (info->kind == harness::FileKind::Symlink) {
            auto canonical = fs.canonicalPath(marker.path);
            if (!canonical) {
                result.diagnostics.push_back(diagnostic(
                    ResourceDiagnosticSeverity::Warning,
                    "marker_symlink_invalid",
                    canonical.error().message,
                    marker.path,
                    marker.kind));
                loadable = false;
            } else if (!kind_matches(canonical_kind(*canonical), marker.expected_kind)) {
                result.diagnostics.push_back(diagnostic(
                    ResourceDiagnosticSeverity::Warning,
                    "marker_kind_mismatch",
                    "resource marker has unexpected kind: " + std::string(marker.path),
                    marker.path,
                    marker.kind));
                loadable = false;
            }
        } else if (!kind_matches(info->kind, marker.expected_kind)) {
            result.diagnostics.push_back(diagnostic(
                ResourceDiagnosticSeverity::Warning,
                "marker_kind_mismatch",
                "resource marker has unexpected kind: " + std::string(marker.path),
                marker.path,
                marker.kind));
            loadable = false;
        }

        result.resources.push_back(DetectedProjectResource{
            .kind = marker.kind,
            .path = marker.path,
            .loadable = loadable,
        });
    }

    return result;
}

bool has_detected_kind(
    const ProjectResourceDetectionResult& detection,
    ProjectResourceKind kind) {
    for (const auto& resource : detection.resources) {
        if (resource.kind == kind) {
            return true;
        }
    }
    return false;
}

bool needs_project_trust_resolution(
    const ProjectResourceDetectionResult& detection,
    const ProjectResourcePolicy& policy) {
    for (const auto& resource : detection.resources) {
        if (!resource.loadable) {
            continue;
        }
        if (!has_implemented_loader(resource.kind)) {
            continue;
        }
        if (enablement_for(policy, resource.kind) == ResourceEnablement::Off) {
            continue;
        }
        return true;
    }
    return false;
}

ProjectResourceLoadPlan build_project_resource_load_plan(
    const ProjectResourceDetectionResult& detection,
    const ProjectResourcePolicy& policy,
    const ProjectTrustResolution& trust) {
    ProjectResourceLoadPlan plan;
    plan.diagnostics = detection.diagnostics;
    plan.project_trust_required = needs_project_trust_resolution(detection, policy);

    for (const auto& resource : detection.resources) {
        ResourceLoadDecision decision{
            .kind = resource.kind,
            .detected = true,
            .allowed = false,
            .reason = ResourceSkipReason::Unsupported,
            .path = resource.path,
            .message = "resource kind is not implemented yet",
        };

        if (!resource.loadable) {
            decision.reason = ResourceSkipReason::DetectionError;
            decision.message = "resource marker could not be validated";
        } else if (!has_implemented_loader(resource.kind)) {
            decision.reason = ResourceSkipReason::Unsupported;
            decision.message = "resource kind is detected but no loader exists yet";
        } else if (enablement_for(policy, resource.kind) == ResourceEnablement::Off) {
            decision.reason = ResourceSkipReason::Disabled;
            decision.message = "resource kind disabled by user policy";
        } else if (trust.decision == ProjectTrustDecision::Trusted) {
            decision.allowed = true;
            decision.reason = ResourceSkipReason::Allowed;
            decision.message = "resource kind allowed";
        } else {
            decision.reason = ResourceSkipReason::Untrusted;
            decision.message = "project is not trusted";
            plan.skipped_for_untrusted = true;
        }

        plan.decisions.push_back(std::move(decision));
    }

    return plan;
}

bool project_skills_allowed(const ProjectResourceLoadPlan& plan) {
    for (const auto& decision : plan.decisions) {
        if (decision.kind == ProjectResourceKind::ProjectSkills && decision.allowed) {
            return true;
        }
    }
    return false;
}

bool project_prompts_allowed(const ProjectResourceLoadPlan& plan) {
    for (const auto& decision : plan.decisions) {
        if (decision.kind == ProjectResourceKind::ProjectPrompts && decision.allowed) {
            return true;
        }
    }
    return false;
}

} // namespace cch::coding_agent

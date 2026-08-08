#include "../../include/cch/coding_agent/ProjectResources.hpp"

#include "../harness/WorkspaceFileSystem.hpp"

#include <array>
#include <filesystem>
#include <optional>
#include <string>

namespace cch::coding_agent {
namespace {

struct MarkerSpec {
    ProjectResourceKind kind;
    const char* path;
    harness::FileKind expected_kind;
};

/// pi `TRUST_REQUIRING_PROJECT_CONFIG_RESOURCES` subset (`core/trust-manager.ts`)
/// without extensions/packages and without `settings.json` (Settings Manager
/// owns that marker). The legacy marker names are deleted with no fallback
/// read.
constexpr std::array<MarkerSpec, 5> kMarkers{{
    {
        .kind = ProjectResourceKind::ProjectSkills,
        .path = ".pi/skills",
        .expected_kind = harness::FileKind::Directory,
    },
    {
        .kind = ProjectResourceKind::ProjectPrompts,
        .path = ".pi/prompts",
        .expected_kind = harness::FileKind::Directory,
    },
    {
        .kind = ProjectResourceKind::ProjectThemes,
        .path = ".pi/themes",
        .expected_kind = harness::FileKind::Directory,
    },
    {
        .kind = ProjectResourceKind::ProjectSystemPrompt,
        .path = ".pi/SYSTEM.md",
        .expected_kind = harness::FileKind::File,
    },
    {
        .kind = ProjectResourceKind::ProjectAppendSystemPrompt,
        .path = ".pi/APPEND_SYSTEM.md",
        .expected_kind = harness::FileKind::File,
    },
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
    ResourceDiagnosticType type,
    std::string message,
    std::string path) {
    return ResourceDiagnostic{
        .type = type,
        .message = std::move(message),
        .path = std::move(path),
        .collision = std::nullopt,
    };
}

} // namespace

std::string_view to_string(ProjectResourceKind kind) {
    switch (kind) {
    case ProjectResourceKind::ProjectSkills:
        return "project_skills";
    case ProjectResourceKind::ProjectPrompts:
        return "project_prompts";
    case ProjectResourceKind::ProjectThemes:
        return "project_themes";
    case ProjectResourceKind::ProjectSystemPrompt:
        return "project_system_prompt";
    case ProjectResourceKind::ProjectAppendSystemPrompt:
        return "project_append_system_prompt";
    }
    return "project_resource";
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
                ResourceDiagnosticType::Warning,
                info.error().message,
                marker.path));
            continue;
        }

        bool loadable = true;
        if (info->kind == harness::FileKind::Symlink) {
            auto canonical = fs.canonicalPath(marker.path);
            if (!canonical) {
                result.diagnostics.push_back(diagnostic(
                    ResourceDiagnosticType::Warning,
                    canonical.error().message,
                    marker.path));
                loadable = false;
            } else if (!kind_matches(canonical_kind(*canonical), marker.expected_kind)) {
                result.diagnostics.push_back(diagnostic(
                    ResourceDiagnosticType::Warning,
                    "resource marker has unexpected kind: " + std::string(marker.path),
                    marker.path));
                loadable = false;
            }
        } else if (!kind_matches(info->kind, marker.expected_kind)) {
            result.diagnostics.push_back(diagnostic(
                ResourceDiagnosticType::Warning,
                "resource marker has unexpected kind: " + std::string(marker.path),
                marker.path));
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
    const ProjectResourceDetectionResult& detection) {
    for (const auto& resource : detection.resources) {
        if (resource.loadable) {
            return true;
        }
    }
    return false;
}

} // namespace cch::coding_agent

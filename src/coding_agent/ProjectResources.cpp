#include <cch/coding_agent/ProjectResources.hpp>

#include "AsyncTask.hpp"
#include "LoaderPath.hpp"
#include "agent/harness/WorkspaceFileSystem.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>

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

[[nodiscard]] harness::FileError detection_aborted_error() {
    return harness::FileError{
            .code = harness::FileErrorCode::Aborted,
            .message = "Operation aborted",
            .path = std::nullopt,
    };
}

[[nodiscard]] bool detection_aborted(const harness::FileError& error) {
    return error.code == harness::FileErrorCode::Aborted;
}

[[nodiscard]] harness::FileError invalid_detection_filesystems(
        std::string message, std::optional<std::string> path = std::nullopt) {
    return harness::FileError{
            .code = harness::FileErrorCode::Invalid,
            .message = std::move(message),
            .path = std::move(path),
    };
}

[[nodiscard]] std::filesystem::path absolute_normalized(const std::filesystem::path& path) {
    std::error_code ec;
    auto absolute = std::filesystem::absolute(path, ec);
    return (ec ? path : absolute).lexically_normal();
}

[[nodiscard]] bool path_contains(const std::filesystem::path& root, const std::filesystem::path& candidate) {
    return strip_workspace_root(absolute_normalized(root), absolute_normalized(candidate).string()).has_value();
}

[[nodiscard]] std::optional<harness::FileError> validate_detection_filesystems(
        const ProjectResourceFileSystems& filesystems) {
    if (!filesystems.workspace) {
        return invalid_detection_filesystems("project resource detection requires a workspace filesystem");
    }
    const auto workspace_root = absolute_normalized(filesystems.workspace->workspace());
    const auto validate_roots = [&](const auto& roots, std::string_view label) -> std::optional<harness::FileError> {
        for (const auto& filesystem : roots) {
            if (!filesystem) {
                return invalid_detection_filesystems(std::string{label} + " contains an empty filesystem capability");
            }
            const auto root = absolute_normalized(filesystem->workspace());
            if (!path_contains(root, workspace_root)) {
                return invalid_detection_filesystems(
                        std::string{label} + " filesystem root is unrelated to the workspace", root.string());
            }
        }
        return std::nullopt;
    };
    if (auto invalid = validate_roots(filesystems.ancestor_roots, "ancestor roots"); invalid) {
        return invalid;
    }
    // A linked worktree's common repository can be a sibling of the
    // workspace. Git capabilities are supplied by composition and therefore
    // cannot be rejected solely because they are not lexical ancestors.
    for (const auto& filesystem : filesystems.git_roots) {
        if (!filesystem) {
            return invalid_detection_filesystems("git roots contains an empty filesystem capability");
        }
    }
    return std::nullopt;
}

[[nodiscard]] detail::AsyncTask<std::optional<std::string>, harness::FileError>
find_project_agents_skills_dir_async_task(const ProjectResourceFileSystems& filesystems,
        const std::filesystem::path& user_agents_skills_dir,
        std::stop_token stop_token,
        std::vector<ResourceDiagnostic>& diagnostics) {
    const auto user_path = absolute_normalized(user_agents_skills_dir);
    std::vector<std::shared_ptr<harness::AsyncFileSystem>> roots;
    const auto add_unique = [&roots](const std::shared_ptr<harness::AsyncFileSystem>& filesystem) {
        if (!filesystem) {
            return;
        }
        const auto root = absolute_normalized(filesystem->workspace());
        const auto duplicate = std::any_of(roots.begin(), roots.end(), [&root](const auto& existing) {
            return absolute_normalized(existing->workspace()) == root;
        });
        if (!duplicate) {
            roots.push_back(filesystem);
        }
    };
    add_unique(filesystems.workspace);
    for (const auto& root : filesystems.ancestor_roots) {
        add_unique(root);
    }
    for (const auto& root : filesystems.git_roots) {
        add_unique(root);
    }

    for (const auto& filesystem : roots) {
        if (stop_token.stop_requested()) {
            co_return std::unexpected(detection_aborted_error());
        }
        const auto root = absolute_normalized(filesystem->workspace());
        const auto agents_skills = root / ".agents" / "skills";
        if (agents_skills == user_path) {
            continue;
        }
        const auto relative = strip_workspace_root(root, agents_skills.string());
        if (!relative) {
            continue;
        }
        auto info = co_await std::move(filesystem->fileInfo(*relative, stop_token));
        if (!info) {
            if (detection_aborted(info.error())) {
                co_return std::unexpected(std::move(info.error()));
            }
            if (info.error().code != harness::FileErrorCode::NotFound) {
                diagnostics.push_back(
                        diagnostic(ResourceDiagnosticType::Warning, info.error().message, agents_skills.string()));
            }
            continue;
        }
        if (info->kind == harness::FileKind::Directory) {
            co_return std::optional<std::string>{agents_skills.string()};
        }
        if (info->kind != harness::FileKind::Symlink) {
            continue;
        }

        // Metadata is no-follow, so resolve a contained symlink explicitly
        // before admitting it as a trust marker. The canonical target is
        // checked through the same capability and must be a directory.
        auto canonical = co_await std::move(filesystem->canonicalPath(*relative, stop_token));
        if (!canonical) {
            if (detection_aborted(canonical.error())) {
                co_return std::unexpected(std::move(canonical.error()));
            }
            diagnostics.push_back(
                    diagnostic(ResourceDiagnosticType::Warning, canonical.error().message, agents_skills.string()));
            continue;
        }
        const auto target_relative = strip_workspace_root(root, *canonical);
        if (!target_relative) {
            diagnostics.push_back(diagnostic(ResourceDiagnosticType::Warning,
                    "project agents skills symlink target is outside the authorized workspace",
                    agents_skills.string()));
            continue;
        }
        auto target_info = co_await std::move(filesystem->fileInfo(*target_relative, stop_token));
        if (!target_info) {
            if (detection_aborted(target_info.error())) {
                co_return std::unexpected(std::move(target_info.error()));
            }
            if (target_info.error().code != harness::FileErrorCode::NotFound) {
                diagnostics.push_back(diagnostic(
                        ResourceDiagnosticType::Warning, target_info.error().message, agents_skills.string()));
            }
            continue;
        }
        if (target_info->kind == harness::FileKind::Directory) {
            co_return std::optional<std::string>{agents_skills.string()};
        }
    }
    co_return std::optional<std::string>{};
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
    case ProjectResourceKind::ProjectAgentsSkills:
        return "project_agents_skills";
    }
    return "project_resource";
}

ProjectResourceDetectionResult detect_project_resources(
    const harness::WorkspaceFileSystem& fs,
    const std::filesystem::path& user_agents_skills_dir) {
    // Temporary expand-contract bridge: all detection now runs through the
    // canonical asynchronous capability. Ancestor capabilities are bounded
    // to the known path chain so the bridge preserves the legacy
    // `.agents/skills` trust-marker search without widening authority.
    ProjectResourceFileSystems filesystems;
    filesystems.workspace = detail::make_sync_async_filesystem(fs);
    auto current = absolute_normalized(fs.root());
    while (true) {
        const auto parent = current.parent_path();
        if (parent == current || parent == parent.root_path()) {
            break;
        }
        current = parent;
        if (auto ancestor = harness::WorkspaceFileSystem::create(current)) {
            filesystems.ancestor_roots.push_back(
                detail::make_sync_async_filesystem(*ancestor));
        }
    }

    auto completed = detail::run_sync_bridge(detect_project_resources(
        std::move(filesystems), user_agents_skills_dir));
    if (!completed) {
        return ProjectResourceDetectionResult{};
    }
    if (!*completed) {
        ProjectResourceDetectionResult result;
        result.diagnostics.push_back(diagnostic(
            ResourceDiagnosticType::Warning,
            completed->error().message,
            completed->error().path.value_or(std::string{})));
        return result;
    }
    return std::move(**completed);
}

namespace {

[[nodiscard]] detail::AsyncTask<ProjectResourceDetectionResult, harness::FileError> detect_project_resources_task(
        ProjectResourceFileSystems filesystems,
        std::filesystem::path user_agents_skills_dir,
        std::stop_token stop_token) {
    ProjectResourceDetectionResult result;
    if (auto invalid = validate_detection_filesystems(filesystems); invalid) {
        co_return std::unexpected(std::move(*invalid));
    }
    for (const auto& marker : kMarkers) {
        if (stop_token.stop_requested()) {
            co_return std::unexpected(detection_aborted_error());
        }
        auto info = co_await std::move(filesystems.workspace->fileInfo(marker.path, stop_token));
        if (!info) {
            if (detection_aborted(info.error())) {
                co_return std::unexpected(std::move(info.error()));
            }
            if (info.error().code == harness::FileErrorCode::NotFound) {
                continue;
            }
            result.diagnostics.push_back(
                    diagnostic(ResourceDiagnosticType::Warning, info.error().message, marker.path));
            continue;
        }

        bool loadable = true;
        harness::FileKind actual_kind = info->kind;
        if (actual_kind == harness::FileKind::Symlink) {
            auto canonical = co_await std::move(filesystems.workspace->canonicalPath(marker.path, stop_token));
            if (!canonical) {
                if (detection_aborted(canonical.error())) {
                    co_return std::unexpected(std::move(canonical.error()));
                }
                result.diagnostics.push_back(
                        diagnostic(ResourceDiagnosticType::Warning, canonical.error().message, marker.path));
                loadable = false;
            } else {
                const auto target_path =
                        strip_workspace_root(absolute_normalized(filesystems.workspace->workspace()), *canonical);
                if (!target_path) {
                    result.diagnostics.push_back(diagnostic(ResourceDiagnosticType::Warning,
                            "resource marker symlink target is outside the authorized workspace",
                            marker.path));
                    loadable = false;
                } else {
                    auto target = co_await std::move(filesystems.workspace->fileInfo(*target_path, stop_token));
                    if (!target) {
                        if (detection_aborted(target.error())) {
                            co_return std::unexpected(std::move(target.error()));
                        }
                        result.diagnostics.push_back(
                                diagnostic(ResourceDiagnosticType::Warning, target.error().message, marker.path));
                        loadable = false;
                    } else {
                        actual_kind = target->kind;
                    }
                }
            }
        }
        if (loadable && !kind_matches(actual_kind, marker.expected_kind)) {
            result.diagnostics.push_back(diagnostic(ResourceDiagnosticType::Warning,
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

    auto found = co_await std::move(to_async_result(find_project_agents_skills_dir_async_task(
            filesystems, std::move(user_agents_skills_dir), stop_token, result.diagnostics)));
    if (!found) {
        co_return std::unexpected(std::move(found.error()));
    }
    if (*found) {
        result.resources.push_back(DetectedProjectResource{
                .kind = ProjectResourceKind::ProjectAgentsSkills,
                .path = std::move(**found),
                .loadable = true,
        });
    }
    co_return result;
}

} // namespace

support::AsyncResult<ProjectResourceDetectionResult, harness::FileError> detect_project_resources(
        ProjectResourceFileSystems filesystems,
        const std::filesystem::path& user_agents_skills_dir,
        std::stop_token stop_token) {
    return detail::to_async_result(
            detect_project_resources_task(std::move(filesystems), user_agents_skills_dir, stop_token));
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

#include "coding_agent/ProjectResourceLoader.hpp"

#include "AsyncTask.hpp"
#include "LoaderPath.hpp"
#include <cch/coding_agent/AgentConfigDir.hpp>
#include <cch/support/Error.hpp>

#include "coding_agent/PromptTemplateLoader.hpp"
#include "coding_agent/ResourceDiagnosticPolicy.hpp"
#include "coding_agent/SkillLoader.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace cch::coding_agent {
using detail::to_async_result;
using detail::warning_diagnostic;
namespace {

/// pi `loadContextFileFromDir` candidate names (`resource-loader.ts`), in
/// order; the first existing regular file wins.
constexpr std::array<std::string_view, 4> kContextFileCandidates{
    "AGENTS.md", "AGENTS.MD", "CLAUDE.md", "CLAUDE.MD"};

/// pi `String.prototype.trim()`.
[[nodiscard]] std::string_view trim(std::string_view text) {
    while (!text.empty() &&
           std::isspace(static_cast<unsigned char>(text.front()))) {
        text.remove_prefix(1);
    }
    while (!text.empty() &&
           std::isspace(static_cast<unsigned char>(text.back()))) {
        text.remove_suffix(1);
    }
    return text;
}

[[nodiscard]] ResourceDiagnostic error_diagnostic(
    std::string message,
    std::optional<std::string> path = std::nullopt) {
    return ResourceDiagnostic{
        .type = ResourceDiagnosticType::Error,
        .message = std::move(message),
        .path = std::move(path),
        .collision = std::nullopt,
    };
}

/// One prompt/skill/theme diagnostic producer: appends into both the merged
/// (call-order) sequence and the producer's per-kind bucket (pi buckets
/// diagnostics per resource kind; the merged sequence keeps pi's trust →
/// prompts → skills → themes → context → system stderr order, #418).
class KindedDiagnosticSink {
public:
    KindedDiagnosticSink(
        std::vector<ResourceDiagnostic>& merged,
        std::vector<ResourceDiagnostic>& kind)
        : merged_(merged), kind_(kind) {}

    void push(ResourceDiagnostic diagnostic) {
        merged_.push_back(diagnostic);
        kind_.push_back(std::move(diagnostic));
    }

private:
    std::vector<ResourceDiagnostic>& merged_;
    std::vector<ResourceDiagnostic>& kind_;
};

/// Tracks loaded skill names to their first-seen source path so that
/// cross-source duplicates produce pi-shaped collision diagnostics with
/// winner/loser paths (pi's one `loadSkills` call shares its name map across
/// every source).
class SkillNameTracker {
public:
    /// Appends one source batch, dropping duplicate names (first wins).
    void append(
        std::vector<Skill>& out,
        KindedDiagnosticSink& sink,
        std::vector<Skill> incoming) {
        for (auto& skill : incoming) {
            if (auto it = by_name_.find(skill.name); it != by_name_.end()) {
                sink.push(detail::make_collision_diagnostic("name \"" + skill.name + "\" collision",
                        ResourceCollisionResourceType::Skill,
                        skill.name,
                        it->second,
                        skill.filePath));
                continue;
            }
            by_name_.emplace(skill.name, skill.filePath);
            out.push_back(std::move(skill));
        }
    }

private:
    std::unordered_map<std::string, std::string> by_name_;
};

void add_trust_diagnostics(
    std::vector<ResourceDiagnostic>& diagnostics,
    const ProjectTrustResolution& trust) {
    for (const auto& diagnostic : trust.diagnostics) {
        diagnostics.push_back(warning_diagnostic(
            diagnostic.message,
            diagnostic.path));
    }
}

/// Tracks loaded template names to their first-seen source path so that
/// cross-source duplicates produce pi-shaped collision diagnostics with
/// winner/loser paths.
class PromptNameTracker {
public:
    /// Appends one source batch, dropping duplicate names (first wins).
    void append(
        std::vector<PromptTemplate>& out,
        KindedDiagnosticSink& sink,
        std::vector<PromptTemplate> incoming) {
        for (auto& tmpl : incoming) {
            if (auto it = by_name_.find(tmpl.name); it != by_name_.end()) {
                sink.push(detail::make_collision_diagnostic("name \"/" + tmpl.name + "\" collision",
                        ResourceCollisionResourceType::Prompt,
                        tmpl.name,
                        it->second,
                        tmpl.filePath));
                continue;
            }
            by_name_.emplace(tmpl.name, tmpl.filePath);
            out.push_back(std::move(tmpl));
        }
    }

private:
    std::unordered_map<std::string, std::string> by_name_;
};

void bound_diagnostics(ProjectResourceLoadingResult& result) {
    const auto bound = [](auto& diagnostics) {
        for (auto& diagnostic : diagnostics) {
            detail::bound_resource_diagnostic_text(diagnostic.message);
            if (diagnostic.path) {
                detail::bound_resource_diagnostic_text(*diagnostic.path);
            }
            if (diagnostic.collision) {
                detail::bound_resource_diagnostic_text(diagnostic.collision->name);
                detail::bound_resource_diagnostic_text(diagnostic.collision->winner_path);
                detail::bound_resource_diagnostic_text(diagnostic.collision->loser_path);
                if (diagnostic.collision->winner_source) {
                    detail::bound_resource_diagnostic_text(*diagnostic.collision->winner_source);
                }
                if (diagnostic.collision->loser_source) {
                    detail::bound_resource_diagnostic_text(*diagnostic.collision->loser_source);
                }
            }
        }
    };
    bound(result.diagnostics);
    bound(result.skill_diagnostics);
    bound(result.prompt_diagnostics);
    bound(result.theme_diagnostics);
    bound(result.fatal_errors);

    if (result.diagnostics.size() <= detail::kMaxResourceDiagnostics) {
        return;
    }

    result.diagnostics.resize(detail::kMaxResourceDiagnostics - 1);
    result.diagnostics.push_back(warning_diagnostic("Additional project resource diagnostics were omitted"));
}

} // namespace

namespace {

[[nodiscard]] harness::FileError loader_aborted_error(std::optional<std::string> path = std::nullopt) {
    return harness::FileError{
            .code = harness::FileErrorCode::Aborted,
            .message = "Operation aborted",
            .path = std::move(path),
    };
}

[[nodiscard]] bool loader_aborted(const harness::FileError& error) {
    return error.code == harness::FileErrorCode::Aborted;
}

[[nodiscard]] std::filesystem::path normalized_absolute(const std::filesystem::path& path) {
    std::error_code ec;
    auto absolute = std::filesystem::absolute(path, ec);
    absolute = (ec ? path : std::move(absolute)).lexically_normal();
    while (absolute != absolute.root_path() && absolute.filename().empty()) {
        absolute = absolute.parent_path();
    }
    return absolute;
}

[[nodiscard]] std::string addressed_path(
        const harness::AsyncFileSystem& filesystem, const std::filesystem::path& path) {
    const auto candidate = path.is_absolute() ? path.lexically_normal().string() : path.string();
    if (const auto relative = strip_workspace_root(normalized_absolute(filesystem.workspace()), candidate)) {
        return *relative;
    }
    return candidate;
}

[[nodiscard]] std::string absolute_display_path(
        const harness::AsyncFileSystem& filesystem, const std::filesystem::path& path) {
    if (path.is_absolute()) {
        return path.lexically_normal().string();
    }
    return (filesystem.workspace() / path).lexically_normal().string();
}

[[nodiscard]] bool path_is_under(const std::filesystem::path& root, const std::filesystem::path& candidate) {
    return strip_workspace_root(normalized_absolute(root), normalized_absolute(candidate).string()).has_value();
}

/// Resolve a path discovered from an already-supplied discovery root to the
/// deepest capability containing it (git worktree traversal).
[[nodiscard]] std::shared_ptr<harness::AsyncFileSystem> discovered_filesystem_for_path(
        const ProjectResourceFileSystems& filesystems, const std::filesystem::path& path) {
    const auto candidate = normalized_absolute(path);
    std::shared_ptr<harness::AsyncFileSystem> best;
    std::size_t best_length{0};
    const auto consider = [&](const std::shared_ptr<harness::AsyncFileSystem>& filesystem) {
        if (!filesystem || !path_is_under(filesystem->workspace(), candidate)) {
            return;
        }
        const auto length = normalized_absolute(filesystem->workspace()).string().size();
        if (!best || length > best_length) {
            best = filesystem;
            best_length = length;
        }
    };
    consider(filesystems.workspace);
    consider(filesystems.agent_config_directory);
    consider(filesystems.user_agents_root);
    for (const auto& filesystem : filesystems.ancestor_roots) {
        consider(filesystem);
    }
    for (const auto& filesystem : filesystems.git_roots) {
        consider(filesystem);
    }
    return best;
}

[[nodiscard]] harness::FileError invalid_filesystem_collection(
        std::string message, std::optional<std::string> path = std::nullopt) {
    return harness::FileError{
            .code = harness::FileErrorCode::Invalid,
            .message = std::move(message),
            .path = std::move(path),
    };
}

[[nodiscard]] std::optional<harness::FileError> validate_resource_filesystems(
        const ProjectResourceFileSystems& filesystems, const ProjectResourceLoadingRequest& request) {
    if (!filesystems.workspace) {
        return invalid_filesystem_collection("project resource loading requires a workspace filesystem");
    }

    const auto workspace_root = normalized_absolute(filesystems.workspace->workspace());
    const auto requested_workspace =
            normalized_absolute(request.workspace.empty() ? workspace_root : request.workspace);
    if (workspace_root != requested_workspace) {
        return invalid_filesystem_collection(
                "workspace filesystem root does not match the resource request", workspace_root.string());
    }

    const auto check_labeled_root = [](const std::shared_ptr<harness::AsyncFileSystem>& filesystem,
                                            const std::optional<std::filesystem::path>& requested,
                                            std::string_view label) -> std::optional<harness::FileError> {
        if (!filesystem) {
            return std::nullopt;
        }
        if (!requested || requested->empty()) {
            return invalid_filesystem_collection(
                    std::string{label} + " filesystem supplied without a matching request path",
                    normalized_absolute(filesystem->workspace()).string());
        }
        const auto actual = normalized_absolute(filesystem->workspace());
        const auto expected = normalized_absolute(*requested);
        if (actual != expected) {
            return invalid_filesystem_collection(
                    std::string{label} + " filesystem root does not match the resource request", actual.string());
        }
        return std::nullopt;
    };

    if (auto invalid = check_labeled_root(
                filesystems.agent_config_directory, request.agent_config_directory, "agent config");
            invalid) {
        return invalid;
    }

    const auto home = normalized_absolute(request.home_directory.value_or(home_directory()));
    const auto expected_user_agents_root = home / ".agents";
    if (filesystems.user_agents_root) {
        const auto actual = normalized_absolute(filesystems.user_agents_root->workspace());
        if (actual != expected_user_agents_root) {
            return invalid_filesystem_collection(
                    "user agents filesystem root does not match the resource request", actual.string());
        }
    }

    const auto check_ancestor_roots = [&](const auto& roots,
                                              std::string_view label) -> std::optional<harness::FileError> {
        for (const auto& filesystem : roots) {
            if (!filesystem) {
                return invalid_filesystem_collection(std::string{label} + " contains an empty filesystem capability");
            }
            const auto root = normalized_absolute(filesystem->workspace());
            if (!path_is_under(root, workspace_root)) {
                return invalid_filesystem_collection(
                        std::string{label} + " filesystem root is unrelated to the workspace", root.string());
            }
        }
        return std::nullopt;
    };
    if (auto invalid = check_ancestor_roots(filesystems.ancestor_roots, "ancestor roots"); invalid) {
        return invalid;
    }
    // A linked worktree's common git repository is often a sibling of the
    // worktree rather than one of its lexical ancestors, so validate presence
    // without imposing the ancestor relationship used by context roots.
    for (const auto& filesystem : filesystems.git_roots) {
        if (!filesystem) {
            return invalid_filesystem_collection("git roots contains an empty filesystem capability");
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::vector<std::shared_ptr<harness::AsyncFileSystem>> ordered_context_filesystems(
        const ProjectResourceFileSystems& filesystems) {
    std::vector<std::shared_ptr<harness::AsyncFileSystem>> roots;
    const auto add_unique = [&](const std::shared_ptr<harness::AsyncFileSystem>& filesystem) {
        if (!filesystem) {
            return;
        }
        const auto root = normalized_absolute(filesystem->workspace());
        for (const auto& existing : roots) {
            if (normalized_absolute(existing->workspace()) == root) {
                return;
            }
        }
        roots.push_back(filesystem);
    };
    add_unique(filesystems.workspace);
    for (const auto& filesystem : filesystems.ancestor_roots) {
        add_unique(filesystem);
    }
    for (const auto& filesystem : filesystems.git_roots) {
        add_unique(filesystem);
    }
    std::stable_sort(roots.begin(), roots.end(), [](const auto& left, const auto& right) {
        return normalized_absolute(left->workspace()).string().size() <
               normalized_absolute(right->workspace()).string().size();
    });
    return roots;
}

void append_skill_load_diagnostics(KindedDiagnosticSink& sink, SkillLoadResult loaded) {
    for (auto& diagnostic : loaded.diagnostics) {
        const auto type =
                diagnostic.type == "collision" ? ResourceDiagnosticType::Collision : ResourceDiagnosticType::Warning;
        sink.push(ResourceDiagnostic{
                .type = type,
                .message = std::move(diagnostic.message),
                .path = diagnostic.path.empty() ? std::nullopt : std::optional<std::string>{std::move(diagnostic.path)},
                .collision = std::move(diagnostic.collision),
        });
    }
}

} // namespace

namespace {

struct AsyncContextFileLoad {
    std::optional<prompt::ProjectContextFile> file;
    std::vector<ResourceDiagnostic> diagnostics;
};

struct AsyncGitPaths {
    std::filesystem::path worktree_root;
    std::filesystem::path common_git_dir;
    std::shared_ptr<harness::AsyncFileSystem> worktree_filesystem;
};

[[nodiscard]] detail::AsyncTask<std::optional<AsyncGitPaths>, harness::FileError> find_git_paths_task(
        const ProjectResourceFileSystems& filesystems,
        const std::filesystem::path& cwd,
        std::stop_token stop_token,
        std::vector<ResourceDiagnostic>& diagnostics) {
    auto roots = ordered_context_filesystems(filesystems);
    std::stable_sort(roots.begin(), roots.end(), [](const auto& left, const auto& right) {
        return normalized_absolute(left->workspace()).string().size() >
               normalized_absolute(right->workspace()).string().size();
    });
    const auto normalized_cwd = normalized_absolute(cwd);
    for (const auto& filesystem : roots) {
        const auto root = normalized_absolute(filesystem->workspace());
        if (!path_is_under(root, normalized_cwd)) {
            continue;
        }
        auto info = co_await std::move(filesystem->fileInfo(".git", stop_token));
        if (!info) {
            if (loader_aborted(info.error())) {
                co_return std::unexpected(std::move(info.error()));
            }
            continue;
        }
        if (info->kind == harness::FileKind::Directory) {
            auto head = co_await std::move(filesystem->fileInfo(".git/HEAD", stop_token));
            if (!head) {
                if (loader_aborted(head.error())) {
                    co_return std::unexpected(std::move(head.error()));
                }
                continue;
            }
            co_return std::optional<AsyncGitPaths>{AsyncGitPaths{
                    .worktree_root = root,
                    .common_git_dir = root / ".git",
                    .worktree_filesystem = filesystem,
            }};
        }
        if (info->kind != harness::FileKind::File) {
            continue;
        }
        auto content = co_await std::move(filesystem->readTextFile(".git", stop_token));
        if (!content) {
            if (loader_aborted(content.error())) {
                co_return std::unexpected(std::move(content.error()));
            }
            continue;
        }
        const auto git_file = trim(*content);
        if (!git_file.starts_with("gitdir: ")) {
            continue;
        }
        const auto git_dir = std::filesystem::path{git_file.substr(8)}.is_absolute()
                                     ? std::filesystem::path{git_file.substr(8)}
                                     : (root / std::filesystem::path{git_file.substr(8)});
        auto git_filesystem = discovered_filesystem_for_path(filesystems, git_dir);
        if (!git_filesystem) {
            continue;
        }
        auto head = co_await std::move(
                git_filesystem->fileInfo(addressed_path(*git_filesystem, git_dir / "HEAD"), stop_token));
        if (!head) {
            if (loader_aborted(head.error())) {
                co_return std::unexpected(std::move(head.error()));
            }
            continue;
        }
        auto common_git_dir = git_dir;
        auto commondir = co_await std::move(
                git_filesystem->fileInfo(addressed_path(*git_filesystem, git_dir / "commondir"), stop_token));
        if (commondir && commondir->kind == harness::FileKind::File) {
            auto text = co_await std::move(
                    git_filesystem->readTextFile(addressed_path(*git_filesystem, git_dir / "commondir"), stop_token));
            if (!text) {
                if (loader_aborted(text.error())) {
                    co_return std::unexpected(std::move(text.error()));
                }
                continue;
            }
            common_git_dir = (git_dir / std::filesystem::path{trim(*text)}).lexically_normal();
        }
        auto common_filesystem = discovered_filesystem_for_path(filesystems, common_git_dir);
        if (!common_filesystem) {
            continue;
        }
        auto common_head = co_await std::move(
                common_filesystem->fileInfo(addressed_path(*common_filesystem, common_git_dir / "HEAD"), stop_token));
        if (!common_head) {
            if (loader_aborted(common_head.error())) {
                co_return std::unexpected(std::move(common_head.error()));
            }
            continue;
        }
        (void)diagnostics;
        co_return std::optional<AsyncGitPaths>{AsyncGitPaths{
                .worktree_root = root,
                .common_git_dir = std::move(common_git_dir),
                .worktree_filesystem = filesystem,
        }};
    }
    co_return std::optional<AsyncGitPaths>{};
}

[[nodiscard]] detail::AsyncTask<AsyncContextFileLoad, harness::FileError> load_context_file_from_capability_task(
        const std::shared_ptr<harness::AsyncFileSystem>& filesystem, std::stop_token stop_token) {
    AsyncContextFileLoad result;
    if (!filesystem) {
        co_return result;
    }
    const auto root = normalized_absolute(filesystem->workspace());
    for (const auto filename : kContextFileCandidates) {
        const auto path = root / filename;
        auto info = co_await std::move(filesystem->fileInfo(addressed_path(*filesystem, path), stop_token));
        if (!info) {
            if (loader_aborted(info.error())) {
                co_return std::unexpected(std::move(info.error()));
            }
            if (info.error().code == harness::FileErrorCode::ResourceLimit) {
                result.diagnostics.push_back(warning_diagnostic(info.error().message, path.string()));
            }
            continue;
        }
        if (info->kind != harness::FileKind::File) {
            continue;
        }
        auto content = co_await std::move(filesystem->readTextFile(addressed_path(*filesystem, path), stop_token));
        if (!content) {
            if (loader_aborted(content.error())) {
                co_return std::unexpected(std::move(content.error()));
            }
            result.diagnostics.push_back(warning_diagnostic(
                    "Could not read " + path.string() + ": " + content.error().message, path.string()));
            continue;
        }
        result.file = prompt::ProjectContextFile{
                .path = path.string(),
                .content = std::move(*content),
        };
        break;
    }
    co_return result;
}

[[nodiscard]] detail::AsyncTask<std::string, harness::FileError> canonicalized_path_task(
        const ProjectResourceFileSystems& filesystems, std::filesystem::path path, std::stop_token stop_token) {
    auto filesystem = discovered_filesystem_for_path(filesystems, path);
    if (!filesystem) {
        co_return normalized_absolute(path).string();
    }
    auto canonical = co_await std::move(filesystem->canonicalPath(addressed_path(*filesystem, path), stop_token));
    if (!canonical) {
        if (loader_aborted(canonical.error())) {
            co_return std::unexpected(std::move(canonical.error()));
        }
        co_return normalized_absolute(path).string();
    }
    co_return std::move(*canonical);
}

[[nodiscard]] detail::AsyncTask<std::optional<std::filesystem::path>, harness::FileError>
find_shadowed_context_file_task(const ProjectResourceFileSystems& filesystems,
        const std::filesystem::path& cwd,
        std::stop_token stop_token,
        std::vector<ResourceDiagnostic>& diagnostics) {
    auto git_paths =
            co_await std::move(detail::to_async_result(find_git_paths_task(filesystems, cwd, stop_token, diagnostics)));
    if (!git_paths) {
        co_return std::unexpected(std::move(git_paths.error()));
    }
    if (!*git_paths) {
        co_return std::optional<std::filesystem::path>{};
    }

    const auto common_git_dir = normalized_absolute((*git_paths)->common_git_dir);
    const auto worktree_root = normalized_absolute((*git_paths)->worktree_root);
    const auto main_repo_root = common_git_dir.parent_path();
    const auto prefix = main_repo_root.string() + std::filesystem::path::preferred_separator;
    if (worktree_root == main_repo_root || !worktree_root.string().starts_with(prefix)) {
        co_return std::optional<std::filesystem::path>{};
    }

    auto worktree_canonical = co_await std::move(
            detail::to_async_result(canonicalized_path_task(filesystems, worktree_root, stop_token)));
    if (!worktree_canonical) {
        co_return std::unexpected(std::move(worktree_canonical.error()));
    }
    auto common_canonical = co_await std::move(
            detail::to_async_result(canonicalized_path_task(filesystems, common_git_dir, stop_token)));
    if (!common_canonical) {
        co_return std::unexpected(std::move(common_canonical.error()));
    }
    if (!worktree_canonical->starts_with(main_repo_root.string() + std::filesystem::path::preferred_separator)) {
        co_return std::optional<std::filesystem::path>{};
    }

    auto main_git_canonical = co_await std::move(
            detail::to_async_result(canonicalized_path_task(filesystems, main_repo_root / ".git", stop_token)));
    if (!main_git_canonical) {
        co_return std::unexpected(std::move(main_git_canonical.error()));
    }
    if (*main_git_canonical != *common_canonical) {
        co_return std::optional<std::filesystem::path>{};
    }

    auto own_context = co_await std::move(detail::to_async_result(
            load_context_file_from_capability_task((*git_paths)->worktree_filesystem, stop_token)));
    if (!own_context) {
        co_return std::unexpected(std::move(own_context.error()));
    }
    diagnostics.insert(diagnostics.end(),
            std::make_move_iterator(own_context->diagnostics.begin()),
            std::make_move_iterator(own_context->diagnostics.end()));
    if (!own_context->file) {
        co_return std::optional<std::filesystem::path>{};
    }
    co_return std::optional<std::filesystem::path>{
            main_repo_root / std::filesystem::path{own_context->file->path}.filename()};
}

[[nodiscard]] detail::AsyncTask<std::vector<prompt::ProjectContextFile>, harness::FileError>
load_project_context_files_task(const ProjectResourceFileSystems& filesystems,
        bool no_context_files,
        std::stop_token stop_token,
        std::vector<ResourceDiagnostic>& diagnostics) {
    std::vector<prompt::ProjectContextFile> context_files;
    if (no_context_files) {
        co_return context_files;
    }
    std::set<std::string> seen_paths;
    if (filesystems.agent_config_directory) {
        auto loaded = co_await std::move(to_async_result(
                load_context_file_from_capability_task(filesystems.agent_config_directory, stop_token)));
        if (!loaded) {
            co_return std::unexpected(std::move(loaded.error()));
        }
        diagnostics.insert(diagnostics.end(),
                std::make_move_iterator(loaded->diagnostics.begin()),
                std::make_move_iterator(loaded->diagnostics.end()));
        if (loaded->file) {
            seen_paths.insert(loaded->file->path);
            context_files.push_back(std::move(*loaded->file));
        }
    }

    std::optional<std::filesystem::path> shadowed_context;
    if (filesystems.workspace) {
        auto shadowed = co_await std::move(detail::to_async_result(find_shadowed_context_file_task(
                filesystems, filesystems.workspace->workspace(), stop_token, diagnostics)));
        if (!shadowed) {
            co_return std::unexpected(std::move(shadowed.error()));
        }
        shadowed_context = std::move(*shadowed);
    }
    std::optional<std::string> shadowed_canonical;
    if (shadowed_context) {
        auto canonical = co_await std::move(
                detail::to_async_result(canonicalized_path_task(filesystems, *shadowed_context, stop_token)));
        if (!canonical) {
            co_return std::unexpected(std::move(canonical.error()));
        }
        shadowed_canonical = std::move(*canonical);
    }

    auto roots = ordered_context_filesystems(filesystems);
    for (const auto& filesystem : roots) {
        auto loaded =
                co_await std::move(to_async_result(load_context_file_from_capability_task(filesystem, stop_token)));
        if (!loaded) {
            co_return std::unexpected(std::move(loaded.error()));
        }
        diagnostics.insert(diagnostics.end(),
                std::make_move_iterator(loaded->diagnostics.begin()),
                std::make_move_iterator(loaded->diagnostics.end()));
        if (!loaded->file || seen_paths.contains(loaded->file->path)) {
            continue;
        }
        if (shadowed_canonical) {
            auto canonical = co_await std::move(
                    detail::to_async_result(canonicalized_path_task(filesystems, loaded->file->path, stop_token)));
            if (!canonical) {
                co_return std::unexpected(std::move(canonical.error()));
            }
            if (*canonical == *shadowed_canonical) {
                continue;
            }
        }
        seen_paths.insert(loaded->file->path);
        context_files.push_back(std::move(*loaded->file));
    }
    // The ancestor roots are sorted root-most first. The global context file
    // remains first, while ancestor files are already in their required order.
    co_return context_files;
}

[[nodiscard]] detail::AsyncTask<std::optional<std::string>, harness::FileError> find_existing_path_task(
        const std::shared_ptr<harness::AsyncFileSystem>& filesystem,
        std::filesystem::path path,
        std::stop_token stop_token) {
    if (!filesystem) {
        co_return std::optional<std::string>{};
    }
    auto exists = co_await std::move(filesystem->exists(addressed_path(*filesystem, path), stop_token));
    if (!exists) {
        if (loader_aborted(exists.error())) {
            co_return std::unexpected(std::move(exists.error()));
        }
        co_return std::optional<std::string>{};
    }
    if (!*exists) {
        co_return std::optional<std::string>{};
    }
    co_return std::optional<std::string>{absolute_display_path(*filesystem, path)};
}

struct AsyncResolvedPrompt {
    std::optional<std::string> text;
    std::optional<std::string> source_path;
};

/// pi `resolvePromptInput`: a CLI/discovered value is file content when it
/// names an existing file and literal text otherwise. The value resolves once
/// through the workspace capability's uniform pi `resolveToCwd` resolution
/// (ADR 0057) — "@"/"~" prefixes and relative inputs included — and the
/// resolved absolute path is read directly; there is no authorization list.
[[nodiscard]] detail::AsyncTask<AsyncResolvedPrompt, harness::FileError> resolve_prompt_input_task(
        const ProjectResourceFileSystems& filesystems,
        std::optional<std::string> input,
        std::string description,
        std::vector<ResourceDiagnostic>& diagnostics,
        std::stop_token stop_token) {
    AsyncResolvedPrompt result;
    if (!input || input->empty()) {
        co_return result;
    }

    auto resolved = co_await std::move(filesystems.workspace->absolutePath(*input, stop_token));
    if (!resolved) {
        if (loader_aborted(resolved.error())) {
            co_return std::unexpected(std::move(resolved.error()));
        }
        // A value that cannot resolve as a path is literal text.
        result.text = std::move(input);
        co_return result;
    }
    auto exists = co_await std::move(filesystems.workspace->exists(*resolved, stop_token));
    if (!exists) {
        if (loader_aborted(exists.error())) {
            co_return std::unexpected(std::move(exists.error()));
        }
        result.text = std::move(input);
        co_return result;
    }
    if (!*exists) {
        result.text = std::move(input);
        co_return result;
    }
    result.source_path = *resolved;
    auto content = co_await std::move(filesystems.workspace->readTextFile(*resolved, stop_token));
    if (!content) {
        if (loader_aborted(content.error())) {
            co_return std::unexpected(std::move(content.error()));
        }
        diagnostics.push_back(warning_diagnostic(
                "Could not read " + description + " file " + *input + ": " + content.error().message, *input));
        result.text = std::move(input);
        co_return result;
    }
    result.text = std::move(*content);
    co_return result;
}

} // namespace

namespace {

void append_prompt_load_diagnostics(KindedDiagnosticSink& sink,
        std::vector<PromptTemplateDiagnostic> diagnostics,
        const std::shared_ptr<harness::AsyncFileSystem>& filesystem,
        bool absolute_paths) {
    for (auto& diagnostic : diagnostics) {
        std::string path = std::move(diagnostic.path);
        if (absolute_paths && filesystem && !path.empty()) {
            path = absolute_display_path(*filesystem, path);
        }
        sink.push(warning_diagnostic(std::move(diagnostic.message), std::move(path)));
    }
}

[[nodiscard]] PromptTemplateDirSpec make_prompt_dir_spec(
        std::string path, bool is_file, std::string source, SourceScope scope, std::optional<std::string> base_dir) {
    PromptTemplateDirSpec spec;
    spec.path = std::move(path);
    spec.is_file = is_file;
    SourceInfo source_info;
    source_info.source = std::move(source);
    source_info.scope = scope;
    source_info.base_dir = std::move(base_dir);
    spec.source_info = std::move(source_info);
    return spec;
}

[[nodiscard]] SkillDirSpec make_skill_dir_spec(std::string path,
        bool include_root_files,
        std::string source,
        SourceScope scope,
        std::optional<std::string> base_dir) {
    SkillDirSpec spec;
    spec.path = std::move(path);
    spec.include_root_files = include_root_files;
    spec.source_context.source = std::move(source);
    spec.source_context.scope = scope;
    spec.source_context.base_dir = std::move(base_dir);
    return spec;
}

// The themes vector and KindedDiagnosticSink reference are owned by the awaiting
// load_project_resources_task coroutine frame and must outlive every co_await;
// this helper is awaited inline before the owner frame returns.
[[nodiscard]] detail::AsyncTask<bool, harness::FileError> load_discovered_themes_task(
        std::shared_ptr<harness::AsyncFileSystem> filesystem,
        std::string directory_path,
        std::string stored_path_root,
        SourceScope scope,
        std::vector<LoadedThemeResource>& themes,
        KindedDiagnosticSink& theme_sink,
        std::stop_token stop_token) {
    auto listed = co_await std::move(filesystem->listDir(directory_path, stop_token));
    if (!listed) {
        if (loader_aborted(listed.error())) co_return std::unexpected(std::move(listed.error()));
        if (listed.error().code != harness::FileErrorCode::NotFound)
            theme_sink.push(warning_diagnostic(listed.error().message, stored_path_root));
        co_return true;
    }
    std::ranges::sort(*listed, [](const auto& left, const auto& right) { return left.name < right.name; });
    for (const auto& entry : *listed) {
        if (entry.kind != harness::FileKind::File || !entry.name.ends_with(".json")) continue;
        const auto file_path = directory_path + "/" + entry.name, stored_path = stored_path_root + "/" + entry.name;
        auto content = co_await std::move(filesystem->readTextFile(file_path, stop_token));
        if (!content) {
            if (loader_aborted(content.error())) co_return std::unexpected(std::move(content.error()));
            theme_sink.push(warning_diagnostic(content.error().message, stored_path));
            continue;
        }
        themes.push_back({.path = stored_path, .json = std::move(*content), .scope = scope});
    }
    co_return true;
}

[[nodiscard]] detail::AsyncTask<ProjectResourceLoadingResult, harness::FileError> load_project_resources_task(
        ProjectResourceFileSystems filesystems,
        ProjectTrustStore trust_store,
        ProjectResourceLoadingRequest request,
        std::stop_token stop_token) {
    ProjectResourceLoadingResult result;
    std::vector<ResourceDiagnostic> diagnostics;
    std::vector<ResourceDiagnostic> prompt_diagnostics;
    std::vector<ResourceDiagnostic> skill_diagnostics;
    std::vector<ResourceDiagnostic> theme_diagnostics;
    KindedDiagnosticSink prompt_sink{diagnostics, prompt_diagnostics};
    KindedDiagnosticSink skill_sink{diagnostics, skill_diagnostics};
    KindedDiagnosticSink theme_sink{diagnostics, theme_diagnostics};

    if (auto invalid = validate_resource_filesystems(filesystems, request); invalid) {
        co_return std::unexpected(std::move(*invalid));
    }
    if (stop_token.stop_requested()) {
        co_return std::unexpected(loader_aborted_error());
    }

    const auto workspace = request.workspace.empty() ? normalized_absolute(filesystems.workspace->workspace())
                                                     : normalized_absolute(request.workspace);
    const auto home = request.home_directory.value_or(home_directory());
    const auto user_agents_skills_dir = home / ".agents" / "skills";

    auto detection = co_await std::move(detect_project_resources(filesystems, user_agents_skills_dir, stop_token));
    if (!detection) {
        co_return std::unexpected(std::move(detection.error()));
    }
    result.detection = std::move(*detection);

    result.trust = resolve_project_trust(workspace,
            needs_project_trust_resolution(result.detection),
            trust_store,
            request.default_project_trust,
            request.project_trust_override);
    add_trust_diagnostics(diagnostics, result.trust);
    const bool project_trusted = result.trust.decision == ProjectTrustDecision::Trusted;

    // Prompt templates retain pi's explicit-first, project, then user order.
    // Each explicit input resolves once through the workspace capability's
    // uniform pi `resolveToCwd` resolution (ADR 0057); the resolved absolute
    // path flows into the loader spec and the template's filePath.
    PromptNameTracker prompt_names;
    for (const auto& input : request.explicit_prompt_templates) {
        auto resolved = co_await std::move(filesystems.workspace->absolutePath(input.path, stop_token));
        if (!resolved) {
            if (loader_aborted(resolved.error())) {
                co_return std::unexpected(std::move(resolved.error()));
            }
            result.fatal_errors.push_back(error_diagnostic(resolved.error().message, input.path));
            continue;
        }
        auto loaded = co_await std::move(loadPromptTemplates(*filesystems.workspace,
                std::vector<PromptTemplateDirSpec>{
                        make_prompt_dir_spec(*resolved, input.is_file, "cli", SourceScope::Temporary, std::nullopt)},
                stop_token));
        if (!loaded) {
            if (loader_aborted(loaded.error())) {
                co_return std::unexpected(std::move(loaded.error()));
            }
            result.fatal_errors.push_back(error_diagnostic(loaded.error().message, input.path));
            continue;
        }
        if (loaded->templates.empty() && loaded->diagnostics.empty()) {
            result.fatal_errors.push_back(
                    error_diagnostic("explicit prompt template input contains no loadable .md files", input.path));
            continue;
        }
        auto batch = std::move(*loaded);
        for (auto& diagnostic : batch.diagnostics) {
            result.fatal_errors.push_back(error_diagnostic(std::move(diagnostic.message), std::move(diagnostic.path)));
        }
        prompt_names.append(result.resources.prompt_templates, prompt_sink, std::move(batch.templates));
    }
    if (!request.no_prompt_templates && project_trusted) {
        auto loaded = co_await std::move(loadPromptTemplates(*filesystems.workspace,
                std::vector<PromptTemplateDirSpec>{make_prompt_dir_spec(
                        ".pi/prompts", false, "auto", SourceScope::Project, (workspace / ".pi" / "prompts").string())},
                stop_token));
        if (!loaded) {
            if (loader_aborted(loaded.error())) {
                co_return std::unexpected(std::move(loaded.error()));
            }
            prompt_sink.push(warning_diagnostic(loaded.error().message, ".pi/prompts"));
        } else {
            auto batch = std::move(*loaded);
            append_prompt_load_diagnostics(prompt_sink, std::move(batch.diagnostics), filesystems.workspace, false);
            prompt_names.append(result.resources.prompt_templates, prompt_sink, std::move(batch.templates));
        }
    }
    if (!request.no_prompt_templates && filesystems.agent_config_directory && request.agent_config_directory &&
            !request.agent_config_directory->empty()) {
        auto loaded = co_await std::move(loadPromptTemplates(*filesystems.agent_config_directory,
                std::vector<PromptTemplateDirSpec>{make_prompt_dir_spec("prompts",
                        false,
                        "auto",
                        SourceScope::User,
                        normalized_absolute(*request.agent_config_directory).string())},
                stop_token));
        if (!loaded) {
            if (loader_aborted(loaded.error())) {
                co_return std::unexpected(std::move(loaded.error()));
            }
            prompt_sink.push(warning_diagnostic(
                    loaded.error().message, normalized_absolute(*request.agent_config_directory / "prompts").string()));
        } else {
            auto batch = std::move(*loaded);
            append_prompt_load_diagnostics(
                    prompt_sink, std::move(batch.diagnostics), filesystems.agent_config_directory, true);
            prompt_names.append(result.resources.prompt_templates, prompt_sink, std::move(batch.templates));
        }
    }

    // Skills use one name tracker across all discovery roots, so first-wins
    // precedence and collision diagnostics remain independent of adapters.
    SkillNameTracker skill_names;
    if (!request.no_skills) {
        if (project_trusted) {
            auto loaded = co_await std::move(loadSkills(*filesystems.workspace,
                    std::vector<SkillDirSpec>{make_skill_dir_spec(
                            ".pi/skills", true, "auto", SourceScope::Project, (workspace / ".pi").string())},
                    stop_token));
            if (!loaded) {
                if (loader_aborted(loaded.error())) {
                    co_return std::unexpected(std::move(loaded.error()));
                }
                skill_sink.push(warning_diagnostic(loaded.error().message, ".pi/skills"));
            } else {
                auto batch = std::move(*loaded);
                append_skill_load_diagnostics(
                        skill_sink, SkillLoadResult{.skills = {}, .diagnostics = std::move(batch.diagnostics)});
                skill_names.append(result.resources.skills, skill_sink, std::move(batch.skills));
            }
        }

        std::vector<std::shared_ptr<harness::AsyncFileSystem>> project_roots;
        project_roots.push_back(filesystems.workspace);
        const auto add_project_root = [&project_roots](const std::shared_ptr<harness::AsyncFileSystem>& filesystem) {
            if (!filesystem) {
                return;
            }
            const auto root = normalized_absolute(filesystem->workspace());
            const bool duplicate = std::any_of(project_roots.begin(), project_roots.end(), [&](const auto& existing) {
                return normalized_absolute(existing->workspace()) == root;
            });
            if (!duplicate) {
                project_roots.push_back(filesystem);
            }
        };
        for (const auto& filesystem : filesystems.ancestor_roots) {
            add_project_root(filesystem);
        }
        for (const auto& filesystem : filesystems.git_roots) {
            add_project_root(filesystem);
        }
        std::optional<std::filesystem::path> git_root;
        for (const auto& filesystem : filesystems.git_roots) {
            if (!filesystem) continue;
            const auto candidate = normalized_absolute(filesystem->workspace());
            if (!git_root || candidate.string().size() > git_root->string().size()) {
                git_root = candidate;
            }
        }
        if (!git_root) {
            auto git_paths = co_await std::move(
                    detail::to_async_result(find_git_paths_task(filesystems, workspace, stop_token, diagnostics)));
            if (!git_paths) {
                co_return std::unexpected(std::move(git_paths.error()));
            }
            if (*git_paths) {
                git_root = normalized_absolute((*git_paths)->worktree_root);
            }
        }
        if (project_trusted) {
            for (const auto& filesystem : project_roots) {
                const auto root = normalized_absolute(filesystem->workspace());
                if (root / ".agents" / "skills" == normalized_absolute(user_agents_skills_dir)) {
                    if (git_root && root == *git_root) break;
                    continue;
                }
                auto loaded = co_await std::move(loadSkills(*filesystem,
                        std::vector<SkillDirSpec>{make_skill_dir_spec(
                                ".agents/skills", false, "auto", SourceScope::Project, (root / ".agents").string())},
                        stop_token));
                if (!loaded) {
                    if (loader_aborted(loaded.error())) {
                        co_return std::unexpected(std::move(loaded.error()));
                    }
                    skill_sink.push(warning_diagnostic(loaded.error().message, (root / ".agents" / "skills").string()));
                } else {
                    auto batch = std::move(*loaded);
                    append_skill_load_diagnostics(
                            skill_sink, SkillLoadResult{.skills = {}, .diagnostics = std::move(batch.diagnostics)});
                    skill_names.append(result.resources.skills, skill_sink, std::move(batch.skills));
                }
                if (git_root && root == *git_root) break;
            }
        }
        if (filesystems.agent_config_directory && request.agent_config_directory &&
                !request.agent_config_directory->empty()) {
            auto loaded = co_await std::move(loadSkills(*filesystems.agent_config_directory,
                    std::vector<SkillDirSpec>{make_skill_dir_spec("skills",
                            true,
                            "auto",
                            SourceScope::User,
                            normalized_absolute(*request.agent_config_directory).string())},
                    stop_token));
            if (!loaded) {
                if (loader_aborted(loaded.error())) {
                    co_return std::unexpected(std::move(loaded.error()));
                }
            } else {
                auto batch = std::move(*loaded);
                append_skill_load_diagnostics(
                        skill_sink, SkillLoadResult{.skills = {}, .diagnostics = std::move(batch.diagnostics)});
                skill_names.append(result.resources.skills, skill_sink, std::move(batch.skills));
            }
        }
        if (filesystems.user_agents_root) {
            auto loaded = co_await std::move(loadSkills(*filesystems.user_agents_root,
                    std::vector<SkillDirSpec>{make_skill_dir_spec("skills",
                            false,
                            "auto",
                            SourceScope::User,
                            normalized_absolute(filesystems.user_agents_root->workspace()).string())},
                    stop_token));
            if (!loaded) {
                if (loader_aborted(loaded.error())) {
                    co_return std::unexpected(std::move(loaded.error()));
                }
            } else {
                auto batch = std::move(*loaded);
                append_skill_load_diagnostics(
                        skill_sink, SkillLoadResult{.skills = {}, .diagnostics = std::move(batch.diagnostics)});
                skill_names.append(result.resources.skills, skill_sink, std::move(batch.skills));
            }
        }
    }
    // Explicit --skill paths resolve once through the workspace capability's
    // uniform pi `resolveToCwd` resolution (ADR 0057); the resolved absolute
    // path flows into fileInfo, the loadSkills spec, and the skill's
    // filePath/baseDir.
    for (const auto& path : request.skill_paths) {
        auto resolved = co_await std::move(filesystems.workspace->absolutePath(path, stop_token));
        if (!resolved) {
            if (loader_aborted(resolved.error())) {
                co_return std::unexpected(std::move(resolved.error()));
            }
            skill_sink.push(warning_diagnostic(resolved.error().message, path));
            continue;
        }
        auto info = co_await std::move(filesystems.workspace->fileInfo(*resolved, stop_token));
        if (!info) {
            if (loader_aborted(info.error())) {
                co_return std::unexpected(std::move(info.error()));
            }
            if (info.error().code == harness::FileErrorCode::NotFound) {
                skill_sink.push(warning_diagnostic("skill path does not exist", path));
                skill_sink.push(error_diagnostic("Skill path does not exist", path));
            } else {
                skill_sink.push(warning_diagnostic(info.error().message, path));
            }
            continue;
        }
        auto loaded = co_await std::move(loadSkills(*filesystems.workspace,
                std::vector<SkillDirSpec>{
                        make_skill_dir_spec(*resolved, true, "cli", SourceScope::Temporary, std::nullopt)},
                stop_token));
        if (!loaded) {
            if (loader_aborted(loaded.error())) {
                co_return std::unexpected(std::move(loaded.error()));
            }
            skill_sink.push(warning_diagnostic(loaded.error().message, path));
        } else {
            auto batch = std::move(*loaded);
            append_skill_load_diagnostics(
                    skill_sink, SkillLoadResult{.skills = {}, .diagnostics = std::move(batch.diagnostics)});
            skill_names.append(result.resources.skills, skill_sink, std::move(batch.skills));
        }
    }

    // Themes remain documents here; parsing and name dedupe stay in the TUI
    // package. Directory listings are complete-or-error AsyncFileSystem
    // results, so ResourceLimit never produces a partial theme set.
    if (!request.no_themes && project_trusted) {
        auto loaded = co_await std::move(detail::to_async_result(load_discovered_themes_task(filesystems.workspace,
                ".pi/themes",
                ".pi/themes",
                SourceScope::Project,
                result.resources.themes,
                theme_sink,
                stop_token)));
        if (!loaded) {
            co_return std::unexpected(std::move(loaded.error()));
        }
    }
    if (!request.no_themes && filesystems.agent_config_directory && request.agent_config_directory &&
            !request.agent_config_directory->empty()) {
        auto loaded = co_await std::move(
                detail::to_async_result(load_discovered_themes_task(filesystems.agent_config_directory,
                        "themes",
                        normalized_absolute(*request.agent_config_directory / "themes").string(),
                        SourceScope::User,
                        result.resources.themes,
                        theme_sink,
                        stop_token)));
        if (!loaded) {
            co_return std::unexpected(std::move(loaded.error()));
        }
    }
    // Explicit --theme paths resolve once through the workspace capability's
    // uniform pi `resolveToCwd` resolution (ADR 0057); the resolved absolute
    // path is read and recorded as the theme's source path.
    for (const auto& path : request.theme_paths) {
        auto resolved = co_await std::move(filesystems.workspace->absolutePath(path, stop_token));
        if (!resolved) {
            if (loader_aborted(resolved.error())) {
                co_return std::unexpected(std::move(resolved.error()));
            }
            theme_sink.push(warning_diagnostic(resolved.error().message, path));
            continue;
        }
        auto info = co_await std::move(filesystems.workspace->fileInfo(*resolved, stop_token));
        if (!info) {
            if (loader_aborted(info.error())) {
                co_return std::unexpected(std::move(info.error()));
            }
            if (info.error().code == harness::FileErrorCode::NotFound) {
                theme_sink.push(warning_diagnostic("theme path does not exist", path));
                theme_sink.push(error_diagnostic("Theme path does not exist", path));
            } else {
                theme_sink.push(warning_diagnostic(info.error().message, path));
            }
            continue;
        }
        std::vector<std::string> files;
        if (info->kind == harness::FileKind::Directory) {
            auto listed = co_await std::move(filesystems.workspace->listDir(*resolved, stop_token));
            if (!listed) {
                if (loader_aborted(listed.error())) {
                    co_return std::unexpected(std::move(listed.error()));
                }
                theme_sink.push(warning_diagnostic(listed.error().message, path));
                continue;
            }
            std::sort(listed->begin(), listed->end(), [](const auto& left, const auto& right) {
                return left.name < right.name;
            });
            for (const auto& entry : *listed) {
                if (entry.kind == harness::FileKind::File && entry.name.ends_with(".json")) {
                    files.push_back(*resolved + "/" + entry.name);
                }
            }
        } else if (info->kind == harness::FileKind::File) {
            if (!resolved->ends_with(".json")) {
                theme_sink.push(warning_diagnostic("theme path is not a json file", path));
                continue;
            }
            files.push_back(*resolved);
        } else {
            theme_sink.push(warning_diagnostic("theme path is not a json file", path));
            continue;
        }
        for (const auto& file : files) {
            auto content = co_await std::move(filesystems.workspace->readTextFile(file, stop_token));
            if (!content) {
                if (loader_aborted(content.error())) {
                    co_return std::unexpected(std::move(content.error()));
                }
                theme_sink.push(warning_diagnostic(content.error().message, file));
            } else {
                result.resources.themes.push_back({
                        .path = file,
                        .json = std::move(*content),
                        .scope = SourceScope::Temporary,
                });
            }
        }
    }

    auto context_filesystems = filesystems;
    if (!request.agent_config_directory || request.agent_config_directory->empty()) {
        context_filesystems.agent_config_directory.reset();
    }
    auto contexts = co_await std::move(to_async_result(
            load_project_context_files_task(context_filesystems, request.no_context_files, stop_token, diagnostics)));
    if (!contexts) {
        co_return std::unexpected(std::move(contexts.error()));
    }
    result.resources.agents_files = std::move(*contexts);

    std::optional<std::string> system_source = request.system_prompt;
    if (!system_source && project_trusted) {
        auto discovered = co_await std::move(to_async_result(
                find_existing_path_task(filesystems.workspace, workspace / ".pi/SYSTEM.md", stop_token)));
        if (!discovered) co_return std::unexpected(std::move(discovered.error()));
        if (*discovered) {
            system_source = std::move(**discovered);
        }
    }
    if (!system_source && filesystems.agent_config_directory && request.agent_config_directory &&
            !request.agent_config_directory->empty()) {
        auto discovered = co_await std::move(to_async_result(find_existing_path_task(
                filesystems.agent_config_directory, *request.agent_config_directory / "SYSTEM.md", stop_token)));
        if (!discovered) co_return std::unexpected(std::move(discovered.error()));
        if (*discovered) {
            system_source = std::move(**discovered);
        }
    }
    auto resolved_system = co_await std::move(to_async_result(resolve_prompt_input_task(
            filesystems, std::move(system_source), "system prompt", diagnostics, stop_token)));
    if (!resolved_system) co_return std::unexpected(std::move(resolved_system.error()));
    result.resources.system_prompt = std::move(resolved_system->text);
    result.resources.system_prompt_source = std::move(resolved_system->source_path);

    std::vector<std::string> append_sources;
    if (!request.append_system_prompt.empty()) {
        append_sources = request.append_system_prompt;
    } else {
        if (project_trusted) {
            auto discovered = co_await std::move(to_async_result(
                    find_existing_path_task(filesystems.workspace, workspace / ".pi/APPEND_SYSTEM.md", stop_token)));
            if (!discovered) co_return std::unexpected(std::move(discovered.error()));
            if (*discovered) {
                append_sources.push_back(std::move(**discovered));
            }
        }
        if (append_sources.empty() && filesystems.agent_config_directory && request.agent_config_directory &&
                !request.agent_config_directory->empty()) {
            auto discovered =
                    co_await std::move(to_async_result(find_existing_path_task(filesystems.agent_config_directory,
                            *request.agent_config_directory / "APPEND_SYSTEM.md",
                            stop_token)));
            if (!discovered) co_return std::unexpected(std::move(discovered.error()));
            if (*discovered) {
                append_sources.push_back(std::move(**discovered));
            }
        }
    }
    for (auto& source : append_sources) {
        auto resolved = co_await std::move(to_async_result(resolve_prompt_input_task(
                filesystems, std::optional<std::string>{source}, "append system prompt", diagnostics, stop_token)));
        if (!resolved) co_return std::unexpected(std::move(resolved.error()));
        if (resolved->text) {
            result.resources.append_system_prompt.push_back(std::move(*resolved->text));
        }
        if (resolved->source_path) {
            result.resources.append_system_prompt_sources.push_back(std::move(*resolved->source_path));
        }
    }

    result.diagnostics = std::move(diagnostics);
    result.skill_diagnostics = std::move(skill_diagnostics);
    result.prompt_diagnostics = std::move(prompt_diagnostics);
    result.theme_diagnostics = std::move(theme_diagnostics);
    bound_diagnostics(result);
    co_return result;
}

} // namespace

support::AsyncResult<ProjectResourceLoadingResult, harness::FileError> load_project_resources(
        ProjectResourceFileSystems filesystems,
        const ProjectTrustStore& trust_store,
        ProjectResourceLoadingRequest request,
        std::stop_token stop_token) {
    return detail::to_async_result(
            load_project_resources_task(std::move(filesystems), trust_store, std::move(request), stop_token));
}

} // namespace cch::coding_agent

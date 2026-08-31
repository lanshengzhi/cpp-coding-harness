#include "coding_agent/SkillLoader.hpp"

#include "AsyncTask.hpp"
#include "coding_agent/GitIgnoreMatcher.hpp"
#include "LoaderPath.hpp"
#include "coding_agent/SkillFrontmatterParser.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <regex>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace cch::coding_agent {

namespace {

constexpr std::size_t kMaxNameLength = 64;
constexpr std::size_t kMaxDescriptionLength = 1024;

/// The `.gitignore`/`.ignore`/`.fdignore` filenames read at every directory
/// of the walk (pi `IGNORE_FILE_NAMES`).
constexpr std::array<std::string_view, 3> kIgnoreFileNames{
    ".gitignore",
    ".ignore",
    ".fdignore",
};

/// Validate skill name against Agent Skills standard (pi leniency — no
/// parent-directory match requirement, matching `core/skills.ts`
/// `validateName`). Returns diagnostic messages; empty vector means valid.
[[nodiscard]] std::vector<std::string> validateName(
    const std::string& name) {
    std::vector<std::string> errors;

    if (name.size() > kMaxNameLength) {
        errors.push_back("name exceeds " + std::to_string(kMaxNameLength) +
                         " characters (" + std::to_string(name.size()) + ")");
    }
    // Must be lowercase a-z, 0-9, hyphens only.
    if (!std::all_of(name.begin(), name.end(), [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
        })) {
        errors.push_back(
            "name contains invalid characters "
            "(must be lowercase a-z, 0-9, hyphens only)");
    }
    if (!name.empty() && name.front() == '-') {
        errors.push_back("name must not start with a hyphen");
    }
    if (!name.empty() && name.back() == '-') {
        errors.push_back("name must not end with a hyphen");
    }
    if (name.find("--") != std::string::npos) {
        errors.push_back("name must not contain consecutive hyphens");
    }

    return errors;
}

/// Validate skill description. Empty/missing → reject; >1024 chars → warn.
/// Returns diagnostic messages; empty vector means valid.
[[nodiscard]] std::vector<std::string> validateDescription(
    const std::string& description) {
    std::vector<std::string> errors;
    if (description.empty()) {
        errors.push_back("description is required");
    } else if (description.size() > kMaxDescriptionLength) {
        errors.push_back("description exceeds " +
                         std::to_string(kMaxDescriptionLength) +
                         " characters (" + std::to_string(description.size()) + ")");
    }
    return errors;
}

/// The parent directory basename of a slash-separated path.
[[nodiscard]] std::string parentDirName(const std::string& filePath) {
    const std::size_t lastSlash = filePath.rfind('/');
    if (lastSlash == std::string::npos || lastSlash == 0) {
        return "";
    }
    const std::size_t secondLastSlash = filePath.rfind('/', lastSlash - 1);
    const std::size_t start =
        (secondLastSlash == std::string::npos) ? 0 : secondLastSlash + 1;
    return filePath.substr(start, lastSlash - start);
}

/// pi `SourceScope`/`SourceInfo` construction for a discovered skill
/// (`core/skills.ts` `createSkillSourceInfo` re-expressed through the loader
/// subset's final `sourceInfo` shape).
[[nodiscard]] SourceInfo make_source_info(
    const std::string& filePath,
    const SkillSourceContext& context) {
    return SourceInfo{
        .path = filePath,
        .source = context.source,
        .scope = context.scope,
        .origin = SourceOrigin::TopLevel,
        .base_dir = context.base_dir,
    };
}

/// pi `ResourceDiagnostic` collision for a duplicate skill name: the first
/// loaded skill wins and the loser carries the winner/loser paths.
[[nodiscard]] SkillDiagnostic skill_collision_diagnostic(
    const std::string& name,
    const std::string& winner_path,
    const std::string& loser_path) {
    return SkillDiagnostic{
        .type = "collision",
        .code = SkillDiagnosticCode::collision,
        .message = "name \"" + name + "\" collision",
        .path = loser_path,
        .collision = ResourceCollision{
            .resource_type = ResourceCollisionResourceType::Skill,
            .name = name,
            .winner_path = winner_path,
            .loser_path = loser_path,
            .winner_source = std::nullopt,
            .loser_source = std::nullopt,
        },
    };
}

/// pi `loadSkills` `addSkills` dedupe: the same real file reached twice (via
/// symlink) is skipped silently; a duplicate name drops the loser with the
/// winner/loser collision diagnostic.
void append_unique_skill(
    std::unordered_map<std::string, std::string>& seenNames,
    std::unordered_set<std::string>& seenRealPaths,
    SkillLoadResult& result,
    Skill skill,
    std::optional<std::string> real_path) {
    // Match pi `addSkills`: an already-seen real file is silently skipped
    // before checking names, so a symlink alias does not become a collision.
    if (real_path && seenRealPaths.contains(*real_path)) {
        return;
    }
    if (auto it = seenNames.find(skill.name); it != seenNames.end()) {
        result.diagnostics.push_back(skill_collision_diagnostic(
            skill.name,
            it->second,
            skill.filePath));
        return;
    }
    // The real path joins the set only when the skill is kept, so a collision
    // loser never shadows a later distinct-named skill that shares its real file.
    if (real_path) {
        seenRealPaths.insert(*real_path);
    }
    seenNames.emplace(skill.name, skill.filePath);
    result.skills.push_back(std::move(skill));
}

/// The posix path of `path` relative to `root`, both slash-separated and
/// relative to the same filesystem root. `path` must live under `root`.
[[nodiscard]] std::string relative_posix(std::string_view root, std::string_view path) {
    if (path == root) {
        return {};
    }
    if (root == ".") {
        return path.size() >= 2 && path.starts_with("./") ? std::string{path.substr(2)}
                                                          : std::string{path};
    }
    if (path.starts_with(root) && path.size() > root.size() && path[root.size()] == '/') {
        return std::string{path.substr(root.size() + 1)};
    }
    return std::string{path};
}

} // namespace

namespace {

[[nodiscard]] harness::FileError async_aborted_error(std::string path = {}) {
    return harness::FileError{
            .code = harness::FileErrorCode::Aborted,
            .message = "Operation aborted",
            .path = path.empty() ? std::nullopt : std::optional<std::string>{std::move(path)},
    };
}

[[nodiscard]] bool async_aborted(const harness::FileError& error) {
    return error.code == harness::FileErrorCode::Aborted;
}

[[nodiscard]] std::string async_read_path(const harness::AsyncFileSystem& fs, std::string_view file_path) {
    const auto relative_path = strip_workspace_root(fs.workspace(), file_path);
    return relative_path.value_or(std::string{file_path});
}

[[nodiscard]] std::string async_absolute_path(const harness::AsyncFileSystem& fs, std::string_view file_path) {
    const std::filesystem::path path{file_path};
    if (path.is_absolute()) {
        return path.lexically_normal().string();
    }
    return (fs.workspace() / path).lexically_normal().string();
}

/// Canonical paths are authority-bearing results. A symlink target is usable
/// only when it remains inside the capability root; an absolute target outside
/// the root must never be passed back to a filesystem operation as a fallback.
[[nodiscard]] std::optional<std::string> contained_async_path(
        const harness::AsyncFileSystem& fs, std::string_view file_path) {
    std::error_code ec;
    const auto root = std::filesystem::absolute(fs.workspace(), ec).lexically_normal();
    if (ec) {
        return std::nullopt;
    }

    const std::filesystem::path path{file_path};
    const auto absolute_path = path.is_absolute() ? path.lexically_normal() : (root / path).lexically_normal();
    auto relative_path = strip_workspace_root(root, absolute_path.string());
    if (!relative_path) {
        return std::nullopt;
    }
    if (relative_path->empty()) {
        return std::string{"."};
    }
    return relative_path;
}

[[nodiscard]] detail::AsyncTask<SkillLoadResult, harness::FileError> load_skill_from_file_task(
        harness::AsyncFileSystem& fs,
        std::string read_file_path,
        std::string display_file_path,
        SkillSourceContext source_context,
        std::stop_token stop_token) {
    SkillLoadResult result;
    if (stop_token.stop_requested()) {
        co_return std::unexpected(async_aborted_error(display_file_path));
    }

    const auto read_path = async_read_path(fs, read_file_path);
    const auto absolute_path = async_absolute_path(fs, display_file_path);
    auto content = co_await std::move(fs.readTextFile(read_path, stop_token));
    if (!content) {
        if (async_aborted(content.error())) {
            co_return std::unexpected(std::move(content.error()));
        }
        result.diagnostics.push_back(SkillDiagnostic{
                .type = "warning",
                .code = SkillDiagnosticCode::read_failed,
                .message = content.error().message,
                .path = display_file_path,
                .collision = std::nullopt,
        });
        co_return result;
    }

    auto parsed = parseFrontmatter(*content);
    if (!parsed) {
        result.diagnostics.push_back(SkillDiagnostic{
                .type = "warning",
                .code = SkillDiagnosticCode::parse_failed,
                .message = parsed.error().message,
                .path = display_file_path,
                .collision = std::nullopt,
        });
        co_return result;
    }

    const auto& fields = parsed->fields;
    std::string name;
    if (const auto name_it = fields.find("name"); name_it != fields.end()) {
        name = name_it->second;
    } else {
        name = parentDirName(display_file_path);
    }
    for (const auto& error : validateName(name)) {
        result.diagnostics.push_back(SkillDiagnostic{
                .type = "warning",
                .code = SkillDiagnosticCode::invalid_metadata,
                .message = error,
                .path = display_file_path,
                .collision = std::nullopt,
        });
    }

    std::string description;
    if (const auto description_it = fields.find("description"); description_it != fields.end()) {
        description = description_it->second;
        while (!description.empty() && (description.front() == ' ' || description.front() == '\t')) {
            description.erase(0, 1);
        }
        while (!description.empty() && (description.back() == ' ' || description.back() == '\t')) {
            description.pop_back();
        }
    }

    bool description_empty = false;
    for (const auto& error : validateDescription(description)) {
        description_empty = description_empty || error == "description is required";
        result.diagnostics.push_back(SkillDiagnostic{
                .type = "warning",
                .code = SkillDiagnosticCode::invalid_metadata,
                .message = error,
                .path = display_file_path,
                .collision = std::nullopt,
        });
    }
    if (description_empty) {
        co_return result;
    }

    bool disable_model_invocation = false;
    if (const auto disable_it = fields.find("disable-model-invocation"); disable_it != fields.end()) {
        disable_model_invocation = disable_it->second == "true";
    }

    result.skills.push_back(Skill{
            .name = std::move(name),
            .description = std::move(description),
            .filePath = absolute_path,
            .baseDir = std::filesystem::path{absolute_path}.parent_path().string(),
            .sourceInfo = make_source_info(absolute_path, source_context),
            .disableModelInvocation = disable_model_invocation,
    });
    co_return result;
}

[[nodiscard]] detail::AsyncTask<SkillLoadResult, harness::FileError> load_skills_from_dir_task(
        harness::AsyncFileSystem& fs,
        std::string dir_path,
        std::string display_dir_path,
        const SkillDirSpec& spec,
        std::string root,
        IgnoreMatcher matcher,
        std::stop_token stop_token) {
    SkillLoadResult result;
    if (stop_token.stop_requested()) {
        co_return std::unexpected(async_aborted_error(dir_path));
    }

    const auto prefix = [&]() -> std::string {
        const auto relative = relative_posix(root, display_dir_path);
        return relative.empty() ? std::string{} : relative + "/";
    }();
    const auto addressed_dir = dir_path == "." ? std::string{} : dir_path + "/";
    for (const auto filename : kIgnoreFileNames) {
        auto ignored = co_await std::move(fs.readTextFile(addressed_dir + std::string{filename}, stop_token));
        if (!ignored) {
            if (async_aborted(ignored.error())) {
                co_return std::unexpected(std::move(ignored.error()));
            }
            continue;
        }
        matcher.add_rules(*ignored, prefix);
    }

    auto entries_result = co_await std::move(fs.listDir(dir_path, stop_token));
    if (!entries_result) {
        if (async_aborted(entries_result.error())) {
            co_return std::unexpected(std::move(entries_result.error()));
        }
        result.diagnostics.push_back(SkillDiagnostic{
                .type = "warning",
                .code = SkillDiagnosticCode::list_failed,
                .message = entries_result.error().message,
                .path = dir_path,
                .collision = std::nullopt,
        });
        co_return result;
    }

    auto entries = std::move(*entries_result);
    std::sort(
            entries.begin(), entries.end(), [](const auto& left, const auto& right) { return left.name < right.name; });
    std::unordered_map<std::string, std::string> seen_names;
    std::unordered_set<std::string> seen_real_paths;

    const auto real_path_of =
            [&](std::string path) -> detail::AsyncTask<std::optional<std::string>, harness::FileError> {
        if (stop_token.stop_requested()) {
            co_return std::unexpected(async_aborted_error(path));
        }
        auto canonical = co_await std::move(fs.canonicalPath(async_read_path(fs, path), stop_token));
        if (!canonical) {
            if (async_aborted(canonical.error())) {
                co_return std::unexpected(std::move(canonical.error()));
            }
            co_return std::optional<std::string>{};
        }
        auto contained = contained_async_path(fs, *canonical);
        if (!contained) {
            co_return std::optional<std::string>{};
        }
        co_return std::optional<std::string>{async_absolute_path(fs, *contained)};
    };

    // One SKILL.md per directory, matching pi's traversal order.
    for (const auto& entry : entries) {
        if (entry.name != "SKILL.md") {
            continue;
        }
        std::string actual_file_path = async_read_path(fs, entry.path);
        if (entry.kind == harness::FileKind::Symlink) {
            auto canonical = co_await std::move(fs.canonicalPath(actual_file_path, stop_token));
            if (!canonical) {
                if (async_aborted(canonical.error())) {
                    co_return std::unexpected(std::move(canonical.error()));
                }
                continue;
            }
            auto target_path = contained_async_path(fs, *canonical);
            if (!target_path) {
                continue;
            }
            auto target_info = co_await std::move(fs.fileInfo(*target_path, stop_token));
            if (!target_info) {
                if (async_aborted(target_info.error())) {
                    co_return std::unexpected(std::move(target_info.error()));
                }
                continue;
            }
            if (target_info->kind != harness::FileKind::File) {
                continue;
            }
            actual_file_path = std::move(*target_path);
        } else if (entry.kind != harness::FileKind::File) {
            continue;
        }

        const auto display_file_path = async_absolute_path(fs, display_dir_path + "/" + entry.name);
        const auto relative_path = relative_posix(root, display_dir_path + "/" + entry.name);
        if (matcher.ignores(relative_path, false)) {
            continue;
        }
        auto file_result = co_await std::move(to_async_result(load_skill_from_file_task(
                fs, std::move(actual_file_path), display_file_path, spec.source_context, stop_token)));
        if (!file_result) {
            co_return std::unexpected(std::move(file_result.error()));
        }
        auto real_path = co_await std::move(to_async_result(real_path_of(entry.path)));
        if (!real_path) {
            co_return std::unexpected(std::move(real_path.error()));
        }
        for (auto& skill : file_result->skills) {
            append_unique_skill(seen_names, seen_real_paths, result, std::move(skill), std::move(*real_path));
        }
        result.diagnostics.insert(result.diagnostics.end(),
                std::make_move_iterator(file_result->diagnostics.begin()),
                std::make_move_iterator(file_result->diagnostics.end()));
        co_return result;
    }

    for (const auto& entry : entries) {
        if (entry.name.empty() || entry.name.front() == '.' || entry.name == "node_modules") {
            continue;
        }
        harness::FileKind kind = entry.kind;
        std::string actual_entry_path = async_read_path(fs, entry.path);
        if (kind == harness::FileKind::Symlink) {
            auto canonical = co_await std::move(fs.canonicalPath(actual_entry_path, stop_token));
            if (!canonical) {
                if (async_aborted(canonical.error())) {
                    co_return std::unexpected(std::move(canonical.error()));
                }
                continue;
            }
            auto target_path = contained_async_path(fs, *canonical);
            if (!target_path) {
                continue;
            }
            auto target_info = co_await std::move(fs.fileInfo(*target_path, stop_token));
            if (!target_info) {
                if (async_aborted(target_info.error())) {
                    co_return std::unexpected(std::move(target_info.error()));
                }
                continue;
            }
            kind = target_info->kind;
            actual_entry_path = std::move(*target_path);
        }

        const auto display_entry_path = display_dir_path + "/" + entry.name;
        const auto relative_path = relative_posix(root, display_entry_path);
        if (kind == harness::FileKind::Directory) {
            if (matcher.ignores(relative_path, true)) {
                continue;
            }
            auto child_result = co_await std::move(to_async_result(load_skills_from_dir_task(
                    fs, std::move(actual_entry_path), display_entry_path, spec, root, matcher, stop_token)));
            if (!child_result) {
                co_return std::unexpected(std::move(child_result.error()));
            }
            for (auto& skill : child_result->skills) {
                auto real_path = co_await std::move(to_async_result(real_path_of(skill.filePath)));
                if (!real_path) {
                    co_return std::unexpected(std::move(real_path.error()));
                }
                append_unique_skill(seen_names, seen_real_paths, result, std::move(skill), std::move(*real_path));
            }
            result.diagnostics.insert(result.diagnostics.end(),
                    std::make_move_iterator(child_result->diagnostics.begin()),
                    std::make_move_iterator(child_result->diagnostics.end()));
        } else if (kind == harness::FileKind::File && display_dir_path == root && spec.include_root_files &&
                   entry.name.size() > 3 && entry.name.ends_with(".md") && !matcher.ignores(relative_path, false)) {
            const auto display_file_path = async_absolute_path(fs, display_entry_path);
            auto file_result = co_await std::move(to_async_result(load_skill_from_file_task(
                    fs, std::move(actual_entry_path), display_file_path, spec.source_context, stop_token)));
            if (!file_result) {
                co_return std::unexpected(std::move(file_result.error()));
            }
            auto real_path = co_await std::move(to_async_result(real_path_of(entry.path)));
            if (!real_path) {
                co_return std::unexpected(std::move(real_path.error()));
            }
            for (auto& skill : file_result->skills) {
                append_unique_skill(seen_names, seen_real_paths, result, std::move(skill), std::move(*real_path));
            }
            result.diagnostics.insert(result.diagnostics.end(),
                    std::make_move_iterator(file_result->diagnostics.begin()),
                    std::make_move_iterator(file_result->diagnostics.end()));
        }
    }
    co_return result;
}

[[nodiscard]] detail::AsyncTask<SkillLoadResult, harness::FileError> load_skills_task(
        harness::AsyncFileSystem& fs, std::vector<SkillDirSpec> dirs, std::stop_token stop_token) {
    SkillLoadResult result;
    std::unordered_map<std::string, std::string> seen_names;
    std::unordered_set<std::string> seen_real_paths;

    for (const auto& dir_spec : dirs) {
        if (stop_token.stop_requested()) {
            co_return std::unexpected(async_aborted_error());
        }
        auto dir_path = strip_workspace_root(fs.workspace(), dir_spec.path);
        if (!dir_path) {
            continue;
        }
        auto info = co_await std::move(fs.fileInfo(*dir_path, stop_token));
        if (!info) {
            if (async_aborted(info.error())) {
                co_return std::unexpected(std::move(info.error()));
            }
            if (info.error().code != harness::FileErrorCode::NotFound) {
                result.diagnostics.push_back(SkillDiagnostic{
                        .type = "warning",
                        .code = SkillDiagnosticCode::file_info_failed,
                        .message = info.error().message,
                        .path = *dir_path,
                        .collision = std::nullopt,
                });
            }
            continue;
        }

        harness::FileKind kind = info->kind;
        std::string spec_path = *dir_path;
        if (kind == harness::FileKind::Symlink) {
            auto canonical = co_await std::move(fs.canonicalPath(async_read_path(fs, spec_path), stop_token));
            if (!canonical) {
                if (async_aborted(canonical.error())) {
                    co_return std::unexpected(std::move(canonical.error()));
                }
                continue;
            }
            auto target_path = contained_async_path(fs, *canonical);
            if (!target_path) {
                continue;
            }
            auto target = co_await std::move(fs.fileInfo(*target_path, stop_token));
            if (!target) {
                if (async_aborted(target.error())) {
                    co_return std::unexpected(std::move(target.error()));
                }
                continue;
            }
            kind = target->kind;
            spec_path = std::move(*target_path);
        }

        if (kind == harness::FileKind::File) {
            if (!spec_path.ends_with(".md")) {
                result.diagnostics.push_back(SkillDiagnostic{
                        .type = "warning",
                        .code = SkillDiagnosticCode::invalid_metadata,
                        .message = "skill path is not a markdown file",
                        .path = *dir_path,
                        .collision = std::nullopt,
                });
                continue;
            }
            const auto display_file_path = async_absolute_path(fs, *dir_path);
            auto file_result = co_await std::move(to_async_result(
                    load_skill_from_file_task(fs, spec_path, display_file_path, dir_spec.source_context, stop_token)));
            if (!file_result) {
                co_return std::unexpected(std::move(file_result.error()));
            }
            auto canonical = co_await std::move(fs.canonicalPath(async_read_path(fs, spec_path), stop_token));
            if (!canonical && async_aborted(canonical.error())) {
                co_return std::unexpected(std::move(canonical.error()));
            }
            for (auto& skill : file_result->skills) {
                append_unique_skill(seen_names,
                        seen_real_paths,
                        result,
                        std::move(skill),
                        canonical ? std::optional<std::string>{*canonical} : std::nullopt);
            }
            result.diagnostics.insert(result.diagnostics.end(),
                    std::make_move_iterator(file_result->diagnostics.begin()),
                    std::make_move_iterator(file_result->diagnostics.end()));
            continue;
        }
        if (kind != harness::FileKind::Directory) {
            continue;
        }

        auto dir_result = co_await std::move(to_async_result(
                load_skills_from_dir_task(fs, spec_path, *dir_path, dir_spec, *dir_path, IgnoreMatcher{}, stop_token)));
        if (!dir_result) {
            co_return std::unexpected(std::move(dir_result.error()));
        }
        for (auto& skill : dir_result->skills) {
            auto canonical = co_await std::move(fs.canonicalPath(async_read_path(fs, skill.filePath), stop_token));
            if (!canonical && async_aborted(canonical.error())) {
                co_return std::unexpected(std::move(canonical.error()));
            }
            append_unique_skill(seen_names,
                    seen_real_paths,
                    result,
                    std::move(skill),
                    canonical ? std::optional<std::string>{*canonical} : std::nullopt);
        }
        result.diagnostics.insert(result.diagnostics.end(),
                std::make_move_iterator(dir_result->diagnostics.begin()),
                std::make_move_iterator(dir_result->diagnostics.end()));
    }
    co_return result;
}

} // namespace

support::AsyncResult<SkillLoadResult, harness::FileError> loadSkillFromFile(harness::AsyncFileSystem& fs,
        std::string file_path,
        SkillSourceContext source_context,
        std::stop_token stop_token) {
    return detail::to_async_result(
            load_skill_from_file_task(fs, file_path, file_path, std::move(source_context), stop_token));
}

support::AsyncResult<SkillLoadResult, harness::FileError> loadSkills(
        harness::AsyncFileSystem& fs, std::vector<SkillDirSpec> dirs, std::stop_token stop_token) {
    return detail::to_async_result(load_skills_task(fs, std::move(dirs), stop_token));
}

} // namespace cch::coding_agent

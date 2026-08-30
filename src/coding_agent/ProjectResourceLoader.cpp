#include "coding_agent/ProjectResourceLoader.hpp"

#include "AsyncTask.hpp"
#include "LoaderPath.hpp"
#include <cch/coding_agent/AgentConfigDir.hpp>
#include <cch/support/Error.hpp>

#include "coding_agent/PromptTemplateLoader.hpp"
#include "coding_agent/ResourceDiagnosticPolicy.hpp"
#include "coding_agent/SkillLoader.hpp"
#include "agent/harness/WorkspaceFileSystem.hpp"

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
namespace {

constexpr std::string_view kProjectSkillsMarker = ".pi/skills";
constexpr std::string_view kProjectPromptsMarker = ".pi/prompts";
constexpr std::string_view kProjectThemesMarker = ".pi/themes";

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

[[nodiscard]] ResourceDiagnostic warning_diagnostic(
    std::string message,
    std::optional<std::string> path = std::nullopt) {
    return ResourceDiagnostic{
        .type = ResourceDiagnosticType::Warning,
        .message = std::move(message),
        .path = std::move(path),
        .collision = std::nullopt,
    };
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

/// pi `dedupePrompts` collision diagnostic (`resource-loader.ts`):
/// first loaded template wins; the loser carries winner/loser paths.
[[nodiscard]] ResourceDiagnostic prompt_collision_diagnostic(
    const ResourceCollision& collision) {
    return ResourceDiagnostic{
        .type = ResourceDiagnosticType::Collision,
        .message = "name \"/" + collision.name + "\" collision",
        .path = collision.loser_path,
        .collision = collision,
    };
}

/// pi `ResourceDiagnostic` collision for a duplicate skill name: the first
/// loaded skill wins and the loser carries the winner/loser paths.
[[nodiscard]] ResourceDiagnostic skill_collision_diagnostic(
    const std::string& name,
    const std::string& winner_path,
    const std::string& loser_path) {
    return ResourceDiagnostic{
        .type = ResourceDiagnosticType::Collision,
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
                sink.push(skill_collision_diagnostic(
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
                sink.push(prompt_collision_diagnostic(ResourceCollision{
                    .resource_type = ResourceCollisionResourceType::Prompt,
                    .name = tmpl.name,
                    .winner_path = it->second,
                    .loser_path = tmpl.filePath,
                    .winner_source = std::nullopt,
                    .loser_source = std::nullopt,
                }));
                continue;
            }
            by_name_.emplace(tmpl.name, tmpl.filePath);
            out.push_back(std::move(tmpl));
        }
    }

private:
    std::unordered_map<std::string, std::string> by_name_;
};

void load_skills_adapter(
    const harness::WorkspaceFileSystem& fs,
    ProjectResourceLoadingResult& result,
    KindedDiagnosticSink& sink,
    SkillNameTracker& names,
    SkillDirSpec spec) {
    auto load = loadSkills(fs, {std::move(spec)});
    for (auto& diagnostic : load.diagnostics) {
        auto type = diagnostic.type == "collision"
            ? ResourceDiagnosticType::Collision
            : ResourceDiagnosticType::Warning;
        sink.push(ResourceDiagnostic{
            .type = type,
            .message = std::move(diagnostic.message),
            .path = diagnostic.path.empty() ? std::nullopt
                                            : std::optional<std::string>{std::move(diagnostic.path)},
            .collision = std::move(diagnostic.collision),
        });
    }
    // Cross-source dedupe: pi loads every source through one `loadSkills`
    // call with a shared name map, so the assembly tracks names across the
    // per-root scans the C++ subset needs (first source wins; later
    // duplicates emit the winner/loser collision diagnostic).
    names.append(result.resources.skills, sink, std::move(load.skills));
}

/// User skills from `<agent_config_directory>/skills` — pi discovery mode
/// (`loadSkillsFromDirInternal(..., "user", true)`): root-level `.md` files
/// included, sourceInfo scope user with the agent dir as baseDir. Not
/// trust-gated. A missing agent config directory loads nothing silently.
[[maybe_unused]] void load_user_skills_adapter(
    const std::filesystem::path& agent_config_directory,
    ProjectResourceLoadingResult& result,
    KindedDiagnosticSink& sink,
    SkillNameTracker& names) {
    auto fs = harness::WorkspaceFileSystem::create(agent_config_directory);
    if (!fs) {
        return;
    }
    load_skills_adapter(*fs, result, sink, names, SkillDirSpec{
        .path = "skills",
        .include_root_files = true,
        .source_context = SkillSourceContext{
            .source = "auto",
            .scope = SourceScope::User,
            .base_dir = agent_config_directory.string(),
        },
    });
}

/// pi `findGitRepoRoot` (`core/package-manager.ts`): the nearest ancestor of
/// `start` (inclusive) that contains a `.git` entry, or nullopt when none
/// exists up to the filesystem root.
[[nodiscard]] std::optional<std::filesystem::path> find_git_repo_root(
    const std::filesystem::path& start) {
    std::error_code ec;
    auto dir = std::filesystem::absolute(start, ec);
    if (ec) {
        return std::nullopt;
    }
    dir = dir.lexically_normal();
    while (true) {
        if (std::filesystem::exists(dir / ".git", ec)) {
            return dir;
        }
        const auto parent = dir.parent_path();
        if (parent == dir) {
            return std::nullopt;
        }
        dir = parent;
    }
}

/// The project `.agents/skills` convention ancestors: from the workspace up
/// to the git root (inclusive), each with its own baseDir (the owning
/// `.agents` directory) and scope project, in pi's package-manager order
/// (project `.pi/skills` → project `.agents/skills` → user sources). Loads
/// only while the project is trusted. The ancestor whose `.agents/skills`
/// is the user's own directory is skipped (pi `package-manager.ts`
/// `collectAncestorAgentsSkillDirs` filter).
[[maybe_unused]] void load_project_agents_skills_adapter(
    const harness::WorkspaceFileSystem& /*fs*/,
    const std::filesystem::path& workspace,
    const std::filesystem::path& user_agents_skills_dir,
    bool project_trusted,
    ProjectResourceLoadingResult& result,
    KindedDiagnosticSink& sink,
    SkillNameTracker& names) {
    if (!project_trusted) {
        return;
    }

    // Project ancestors from the workspace up to the git root (inclusive),
    // excluding the user's own directory.
    const auto git_root = find_git_repo_root(workspace);
    auto dir = std::filesystem::absolute(workspace);
    while (true) {
        const auto agents_skills_dir = dir / ".agents" / "skills";
        if (agents_skills_dir != user_agents_skills_dir) {
            if (auto ancestor_fs = harness::WorkspaceFileSystem::create(dir);
                ancestor_fs) {
                load_skills_adapter(*ancestor_fs, result, sink, names, SkillDirSpec{
                    .path = ".agents/skills",
                    .include_root_files = false,
                    .source_context = SkillSourceContext{
                        .source = "auto",
                        .scope = SourceScope::Project,
                        .base_dir = (dir / ".agents").string(),
                    },
                });
            }
        }
        if (git_root && dir == *git_root) {
            break;
        }
        const auto parent = dir.parent_path();
        if (parent == dir) {
            break;
        }
        dir = parent;
    }
}

/// User `~/.agents/skills` (pi discovery mode "agents": nested SKILL.md
/// only, baseDir the `.agents` directory, scope user). Always trusted; loads
/// last in pi's order (after user `~/.pi/agent/skills`).
[[maybe_unused]] void load_user_agents_skills_adapter(
    const std::filesystem::path& user_agents_skills_dir,
    ProjectResourceLoadingResult& result,
    KindedDiagnosticSink& sink,
    SkillNameTracker& names) {
    if (auto user_fs = harness::WorkspaceFileSystem::create(
            user_agents_skills_dir.parent_path());
        user_fs) {
        load_skills_adapter(*user_fs, result, sink, names, SkillDirSpec{
            .path = "skills",
            .include_root_files = false,
            .source_context = SkillSourceContext{
                .source = "auto",
                .scope = SourceScope::User,
                .base_dir = user_agents_skills_dir.parent_path().string(),
            },
        });
    }
}

/// pi `--skill` explicit inputs: repeatable files or directories that load
/// after every discovered source (resource-loader.ts `mergePaths` appends
/// `additionalSkillPaths` last, so discovered skills win name collisions)
/// and stay effective under `--no-skills`. Missing inputs carry pi's two
/// diagnostics (the loader warning and the resource-loader error, both
/// non-fatal).
[[maybe_unused]] void load_explicit_skill_paths(
    const harness::WorkspaceFileSystem& fs,
    const std::vector<std::string>& skill_paths,
    ProjectResourceLoadingResult& result,
    KindedDiagnosticSink& sink,
    SkillNameTracker& names) {
    for (const auto& path : skill_paths) {
        const auto info = fs.fileInfo(path);
        if (!info) {
            if (info.error().code == harness::FileErrorCode::NotFound) {
                sink.push(warning_diagnostic(
                    "skill path does not exist",
                    path));
                sink.push(error_diagnostic(
                    "Skill path does not exist",
                    path));
            } else {
                sink.push(warning_diagnostic(
                    info.error().message,
                    path));
            }
            continue;
        }
        load_skills_adapter(fs, result, sink, names, SkillDirSpec{
            .path = path,
            .include_root_files = true,
            .source_context = SkillSourceContext{
                .source = "cli",
                .scope = SourceScope::Temporary,
                .base_dir = std::nullopt,
            },
        });
    }
}


[[maybe_unused]] void load_project_prompts_adapter(
    const harness::WorkspaceFileSystem& fs,
    ProjectResourceLoadingResult& result,
    KindedDiagnosticSink& sink,
    PromptNameTracker& names) {
    auto load = loadPromptTemplates(
        fs,
        {PromptTemplateDirSpec{
            .path = std::string{kProjectPromptsMarker},
            .is_file = false,
            .source_info = SourceInfo{
                .path = {},
                .source = "auto",
                .scope = SourceScope::Project,
                .origin = SourceOrigin::TopLevel,
                .base_dir = (fs.root() / kProjectPromptsMarker).string(),
            },
        }});
    for (auto& diagnostic : load.diagnostics) {
        sink.push(warning_diagnostic(
            std::move(diagnostic.message),
            std::move(diagnostic.path)));
    }
    names.append(result.resources.prompt_templates, sink, std::move(load.templates));
}

[[maybe_unused]] void load_user_prompts_adapter(
    const std::filesystem::path& agent_config_directory,
    ProjectResourceLoadingResult& result,
    KindedDiagnosticSink& sink,
    PromptNameTracker& names) {
    auto fs = harness::WorkspaceFileSystem::create(agent_config_directory);
    if (!fs) {
        // The Agent Config Directory does not exist (or is unreadable); a
        // fresh setup has no user prompts. Best-effort by contract.
        return;
    }
    auto load = loadPromptTemplates(
        *fs,
        {PromptTemplateDirSpec{
            .path = "prompts",
            .is_file = false,
            .source_info = SourceInfo{
                .path = {},
                .source = "auto",
                .scope = SourceScope::User,
                .origin = SourceOrigin::TopLevel,
                .base_dir = agent_config_directory.string(),
            },
        }});
    for (auto& diagnostic : load.diagnostics) {
        sink.push(warning_diagnostic(
            std::move(diagnostic.message),
            (agent_config_directory / diagnostic.path).string()));
    }
    names.append(result.resources.prompt_templates, sink, std::move(load.templates));
}

/// Trust-gated project themes from `.pi/themes` — pi `resource-loader.ts`
/// `loadThemes` default directory. The loader cannot parse themes (parsing
/// stays in the coding-agent TUI package), so it collects the source
/// documents and reports only path-level diagnostics; name-level dedupe
/// happens in the TUI layer's `discover_themes`.
[[maybe_unused]] void load_project_themes_adapter(
    const harness::WorkspaceFileSystem& fs,
    ProjectResourceLoadingResult& result,
    KindedDiagnosticSink& sink) {
    if (auto listed = fs.listDir(std::string{kProjectThemesMarker}); !listed) {
        if (listed.error().code != harness::FileErrorCode::NotFound) {
            sink.push(warning_diagnostic(
                listed.error().message,
                std::string{kProjectThemesMarker}));
        }
    } else {
        std::sort(listed->begin(), listed->end(), [](const auto& left, const auto& right) {
            return left.name < right.name;
        });
        for (const auto& entry : *listed) {
            if (entry.kind != harness::FileKind::File || !entry.name.ends_with(".json")) continue;
            const auto path = std::string{kProjectThemesMarker} + "/" + entry.name;
            if (auto content = fs.readTextFile(path); !content) {
                sink.push(warning_diagnostic(
                    content.error().message,
                    path));
            } else {
                result.resources.themes.push_back({
                    .path = path,
                    .json = std::move(*content),
                    .scope = SourceScope::Project,
                });
            }
        }
    }
}

/// User themes from `<agent_config_directory>/themes` — pi
/// `resource-loader.ts` `loadThemes` default directory. Not trust-gated. A
/// missing agent config directory (or missing themes directory) loads
/// nothing silently.
[[maybe_unused]] void load_user_themes_adapter(
    const std::filesystem::path& agent_config_directory,
    ProjectResourceLoadingResult& result,
    KindedDiagnosticSink& sink) {
    auto fs = harness::WorkspaceFileSystem::create(agent_config_directory);
    if (!fs) {
        return;
    }
    if (auto listed = fs->listDir("themes"); !listed) {
        if (listed.error().code != harness::FileErrorCode::NotFound) {
            sink.push(warning_diagnostic(
                listed.error().message,
                (agent_config_directory / "themes").string()));
        }
    } else {
        std::sort(listed->begin(), listed->end(), [](const auto& left, const auto& right) {
            return left.name < right.name;
        });
        for (const auto& entry : *listed) {
            if (entry.kind != harness::FileKind::File || !entry.name.ends_with(".json")) continue;
            const auto path = (agent_config_directory / "themes" / entry.name).string();
            if (auto content = fs->readTextFile("themes/" + entry.name); !content) {
                sink.push(warning_diagnostic(
                    content.error().message,
                    path));
            } else {
                result.resources.themes.push_back({
                    .path = path,
                    .json = std::move(*content),
                    .scope = SourceScope::User,
                });
            }
        }
    }
}

/// pi `--theme` explicit inputs (resource-loader.ts `loadThemes` path
/// branch): repeatable files or directories that load after every
/// discovered source (discovered themes win name collisions, pi's merge
/// order) and stay effective under `--no-themes`. Missing inputs carry pi's
/// two diagnostics (the loader warning and the resource-loader error, both
/// non-fatal); a non-JSON file and unreadable inputs are warnings.
[[maybe_unused]] void load_explicit_theme_paths(
    const harness::WorkspaceFileSystem& fs,
    const std::vector<std::string>& theme_paths,
    ProjectResourceLoadingResult& result,
    KindedDiagnosticSink& sink) {
    for (const auto& path : theme_paths) {
        const auto info = fs.fileInfo(path);
        if (!info) {
            if (info.error().code == harness::FileErrorCode::NotFound) {
                sink.push(warning_diagnostic(
                    "theme path does not exist",
                    path));
                sink.push(error_diagnostic(
                    "Theme path does not exist",
                    path));
            } else {
                sink.push(warning_diagnostic(
                    info.error().message,
                    path));
            }
            continue;
        }

        std::vector<std::string> files;
        if (info->kind == harness::FileKind::Directory) {
            if (auto listed = fs.listDir(path); !listed) {
                sink.push(warning_diagnostic(
                    listed.error().message,
                    path));
                continue;
            } else {
                std::sort(listed->begin(), listed->end(), [](const auto& left, const auto& right) {
                    return left.name < right.name;
                });
                for (const auto& entry : *listed) {
                    if (entry.kind != harness::FileKind::File || !entry.name.ends_with(".json")) continue;
                    files.push_back(path + "/" + entry.name);
                }
            }
        } else if (info->kind == harness::FileKind::File) {
            if (!path.ends_with(".json")) {
                sink.push(warning_diagnostic(
                    "theme path is not a json file",
                    path));
                continue;
            }
            files.push_back(path);
        } else {
            sink.push(warning_diagnostic(
                "theme path is not a json file",
                path));
            continue;
        }

        for (const auto& file : files) {
            if (auto content = fs.readTextFile(file); !content) {
                sink.push(warning_diagnostic(
                    content.error().message,
                    file));
            } else {
                result.resources.themes.push_back({
                    .path = file,
                    .json = std::move(*content),
                    .scope = SourceScope::Temporary,
                });
            }
        }
    }
}

/// Loads one explicit `--prompt-template` input. Missing or unreadable
/// explicit inputs are fatal (session assembly must fail consistently);
/// duplicate names resolve through the shared tracker as non-fatal
/// collisions.
[[maybe_unused]] void load_explicit_prompt_template_input(
    const harness::WorkspaceFileSystem& fs,
    ProjectResourceLoadingResult& result,
    KindedDiagnosticSink& sink,
    PromptNameTracker& names,
    ExplicitPromptTemplateInput input) {
    auto input_path = std::move(input.path);
    auto explicit_load = loadPromptTemplates(
        fs,
        {PromptTemplateDirSpec{
            .path = input_path,
            .is_file = input.is_file,
            .source_info = SourceInfo{
                .path = {},
                .source = "cli",
                .scope = SourceScope::Temporary,
                .origin = SourceOrigin::TopLevel,
                .base_dir = std::nullopt,
            },
        }});

    if (explicit_load.templates.empty() && explicit_load.diagnostics.empty()) {
        result.fatal_errors.push_back(error_diagnostic(
            "explicit prompt template input contains no loadable .md files",
            input_path));
        return;
    }

    for (auto& diag : explicit_load.diagnostics) {
        result.fatal_errors.push_back(error_diagnostic(
            std::move(diag.message),
            std::move(diag.path)));
    }

    names.append(result.resources.prompt_templates, sink, std::move(explicit_load.templates));
}

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
    result.diagnostics.push_back(warning_diagnostic(
        "Additional project resource diagnostics were omitted"));
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

[[nodiscard]] std::shared_ptr<harness::AsyncFileSystem> explicit_filesystem_for(
        const ProjectResourceFileSystems& filesystems, const std::filesystem::path& path) {
    const auto candidate = normalized_absolute(path);
    std::shared_ptr<harness::AsyncFileSystem> best;
    std::size_t best_length{0};
    for (const auto& authorized : filesystems.explicit_paths) {
        if (!authorized.filesystem || !path_is_under(authorized.filesystem->workspace(), candidate) ||
                !path_is_under(std::filesystem::path{authorized.path}, candidate)) {
            continue;
        }
        const auto length = normalized_absolute(authorized.path).string().size();
        if (!best || length > best_length) {
            best = authorized.filesystem;
            best_length = length;
        }
    }
    return best;
}

[[nodiscard]] std::shared_ptr<harness::AsyncFileSystem> filesystem_for_path(
        const ProjectResourceFileSystems& filesystems, const std::filesystem::path& path) {
    if (auto explicit_filesystem = explicit_filesystem_for(filesystems, path)) {
        return explicit_filesystem;
    }
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

[[nodiscard]] std::shared_ptr<harness::AsyncFileSystem> explicit_resource_filesystem_for(
        const ProjectResourceFileSystems& filesystems, const std::filesystem::path& path) {
    if (auto explicit_filesystem = explicit_filesystem_for(filesystems, path)) {
        return explicit_filesystem;
    }
    if (filesystems.workspace && path_is_under(filesystems.workspace->workspace(), normalized_absolute(path))) {
        return filesystems.workspace;
    }
    return {};
}

[[nodiscard]] harness::FileError unauthorized_resource_path(const std::filesystem::path& path) {
    return harness::FileError{
            .code = harness::FileErrorCode::PermissionDenied,
            .message = "resource path is not authorized",
            .path = path.string(),
    };
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

class SyncAsyncFileSystemAdapter final : public harness::AsyncFileSystem {
public:
    explicit SyncAsyncFileSystemAdapter(harness::WorkspaceFileSystem filesystem) : filesystem_(std::move(filesystem)) {}

    [[nodiscard]] const std::filesystem::path& workspace() const override { return filesystem_.root(); }

    [[nodiscard]] support::AsyncResult<std::string, harness::FileError> absolutePath(
            std::string path, std::stop_token stop_token) override {
        return immediate<std::string>(path, stop_token, [&] { return filesystem_.absolutePath(path); });
    }

    [[nodiscard]] support::AsyncResult<std::string, harness::FileError> joinPath(
            std::vector<std::string> parts, std::stop_token stop_token) override {
        return immediate<std::string>(std::nullopt, stop_token, [&] { return filesystem_.joinPath(parts); });
    }

    [[nodiscard]] support::AsyncResult<std::string, harness::FileError> readTextFile(
            std::string path, std::stop_token stop_token) override {
        return immediate<std::string>(path, stop_token, [&] { return filesystem_.readTextFile(path); });
    }

    [[nodiscard]] support::AsyncResult<std::vector<std::string>, harness::FileError> readTextLines(
            std::string path, std::optional<int> max_lines, std::stop_token stop_token) override {
        return immediate<std::vector<std::string>>(
                path, stop_token, [&] { return filesystem_.readTextLines(path, max_lines); });
    }

    [[nodiscard]] support::AsyncResult<harness::BinaryData, harness::FileError> readBinaryFile(
            std::string path, std::stop_token stop_token) override {
        return immediate<harness::BinaryData>(path, stop_token, [&] { return filesystem_.readBinaryFile(path); });
    }

    [[nodiscard]] support::AsyncResult<void, harness::FileError> writeFile(
            std::string path, harness::WriteContent content, std::stop_token stop_token) override {
        return immediate<void>(path, stop_token, [&] { return filesystem_.writeFile(path, content); });
    }

    [[nodiscard]] support::AsyncResult<void, harness::FileError> appendFile(
            std::string path, harness::WriteContent content, std::stop_token stop_token) override {
        return immediate<void>(path, stop_token, [&] { return filesystem_.appendFile(path, content); });
    }

    [[nodiscard]] support::AsyncResult<harness::FileInfo, harness::FileError> fileInfo(
            std::string path, std::stop_token stop_token) override {
        return immediate<harness::FileInfo>(path, stop_token, [&] { return filesystem_.fileInfo(path); });
    }

    [[nodiscard]] support::AsyncResult<std::vector<harness::FileInfo>, harness::FileError> listDir(
            std::string path, std::stop_token stop_token) override {
        return immediate<std::vector<harness::FileInfo>>(path, stop_token, [&] { return filesystem_.listDir(path); });
    }

    [[nodiscard]] support::AsyncResult<std::string, harness::FileError> canonicalPath(
            std::string path, std::stop_token stop_token) override {
        return immediate<std::string>(path, stop_token, [&] { return filesystem_.canonicalPath(path); });
    }

    [[nodiscard]] support::AsyncResult<bool, harness::FileError> exists(
            std::string path, std::stop_token stop_token) override {
        return immediate<bool>(path, stop_token, [&] { return filesystem_.exists(path); });
    }

    [[nodiscard]] support::AsyncResult<void, harness::FileError> createDir(
            std::string path, bool recursive, std::stop_token stop_token) override {
        return immediate<void>(path, stop_token, [&] { return filesystem_.createDir(path, recursive); });
    }

    [[nodiscard]] support::AsyncResult<void, harness::FileError> remove(
            std::string path, bool recursive, std::stop_token stop_token) override {
        return immediate<void>(path, stop_token, [&] { return filesystem_.remove(path, recursive); });
    }

    [[nodiscard]] support::AsyncResult<std::string, harness::FileError> createTempDir(
            std::optional<std::string> prefix, std::stop_token stop_token) override {
        return immediate<std::string>(std::nullopt, stop_token, [&] { return filesystem_.createTempDir(prefix); });
    }

    [[nodiscard]] support::AsyncResult<std::string, harness::FileError> createTempFile(
            std::optional<std::string> prefix, std::optional<std::string> suffix, std::stop_token stop_token) override {
        return immediate<std::string>(
                std::nullopt, stop_token, [&] { return filesystem_.createTempFile(prefix, suffix); });
    }

    [[nodiscard]] support::AsyncResult<void, harness::FileError> cleanup() override {
        return support::AsyncResult<void, harness::FileError>{std::expected<void, harness::FileError>{}};
    }

private:
    template <typename Value, typename Operation>
    [[nodiscard]] static support::AsyncResult<Value, harness::FileError> immediate(
            std::optional<std::string> path, std::stop_token stop_token, Operation operation) {
        if (stop_token.stop_requested()) {
            return support::AsyncResult<Value, harness::FileError>{
                    std::unexpected(loader_aborted_error(std::move(path)))};
        }
        return support::AsyncResult<Value, harness::FileError>{operation()};
    }

    harness::WorkspaceFileSystem filesystem_;
};

[[nodiscard]] std::shared_ptr<harness::AsyncFileSystem> make_sync_async_filesystem(const std::filesystem::path& root) {
    auto filesystem = harness::WorkspaceFileSystem::create(root);
    if (!filesystem) {
        return {};
    }
    return std::make_shared<SyncAsyncFileSystemAdapter>(std::move(*filesystem));
}

[[nodiscard]] ProjectResourceFileSystems make_compatibility_filesystems(
        const harness::WorkspaceFileSystem& filesystem, const ProjectResourceLoadingRequest& request) {
    // Temporary expand-contract composition: this bridge owns construction of
    // the old synchronous adapters so the canonical loader itself never
    // creates a capability from a discovered path.
    const auto workspace = normalized_absolute(request.workspace.empty() ? filesystem.root() : request.workspace);
    ProjectResourceFileSystems result;
    result.workspace = make_sync_async_filesystem(workspace);
    if (!result.workspace) {
        result.workspace = std::make_shared<SyncAsyncFileSystemAdapter>(filesystem);
    }

    auto current = workspace;
    std::optional<std::filesystem::path> git_root;
    while (true) {
        // The temporary bridge may authorize known project ancestors, but it
        // must not manufacture a capability rooted at the host filesystem
        // root. That root would turn an unrecognized absolute input into an
        // arbitrary read authority.
        if (current != current.root_path()) {
            if (auto root = make_sync_async_filesystem(current)) {
                result.ancestor_roots.push_back(std::move(root));
            }
        }
        std::error_code git_error;
        if (!git_root && std::filesystem::exists(current / ".git", git_error) && !git_error) {
            git_root = current;
        }
        const auto parent = current.parent_path();
        if (parent == current) break;
        current = parent;
    }
    if (git_root && *git_root != git_root->root_path()) {
        if (auto root = make_sync_async_filesystem(*git_root)) {
            result.git_roots.push_back(std::move(root));
        }
    }

    if (request.agent_config_directory && !request.agent_config_directory->empty()) {
        result.agent_config_directory =
                make_sync_async_filesystem(normalized_absolute(*request.agent_config_directory));
    }
    const auto home = request.home_directory.value_or(home_directory());
    result.user_agents_root = make_sync_async_filesystem(home / ".agents");

    std::vector<std::string> explicit_paths = request.skill_paths;
    explicit_paths.insert(explicit_paths.end(), request.theme_paths.begin(), request.theme_paths.end());
    for (const auto& input : request.explicit_prompt_templates) {
        explicit_paths.push_back(input.path);
    }
    const auto add_prompt_path = [&](const std::optional<std::string>& input) {
        if (input && !input->empty()) explicit_paths.push_back(*input);
    };
    add_prompt_path(request.system_prompt);
    for (const auto& input : request.append_system_prompt) {
        if (!input.empty()) explicit_paths.push_back(input);
    }
    for (const auto& path : explicit_paths) {
        const auto candidate = std::filesystem::path{path};
        if (!candidate.is_absolute()) {
            result.explicit_paths.push_back(AuthorizedResourcePath{
                    .path = (workspace / candidate).lexically_normal().string(),
                    .filesystem = result.workspace,
            });
            continue;
        }
        if (auto cap = filesystem_for_path(result, candidate)) {
            result.explicit_paths.push_back(AuthorizedResourcePath{
                    .path = candidate.lexically_normal().string(),
                    .filesystem = std::move(cap),
            });
        }
    }
    return result;
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
        auto git_filesystem = filesystem_for_path(filesystems, git_dir);
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
        auto common_filesystem = filesystem_for_path(filesystems, common_git_dir);
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
    auto filesystem = filesystem_for_path(filesystems, path);
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

[[nodiscard]] detail::AsyncTask<AsyncResolvedPrompt, harness::FileError> resolve_prompt_input_task(
        const ProjectResourceFileSystems& filesystems,
        std::optional<std::string> input,
        std::string description,
        std::vector<ResourceDiagnostic>& diagnostics,
        bool allow_discovered_path,
        std::stop_token stop_token) {
    AsyncResolvedPrompt result;
    if (!input || input->empty()) {
        co_return result;
    }

    const auto input_path = std::filesystem::path{*input};
    auto filesystem = input_path.is_absolute() ? explicit_resource_filesystem_for(filesystems, input_path)
                                               : filesystems.workspace;
    // Explicit absolute inputs must use the exact path authorization supplied
    // by composition. Discovered SYSTEM/APPEND files may instead use the
    // already-authorized workspace/agent capabilities.
    if (!filesystem && allow_discovered_path) {
        filesystem = filesystem_for_path(filesystems, input_path);
    }
    if (!filesystem) {
        result.text = std::move(input);
        co_return result;
    }
    auto exists = co_await std::move(filesystem->exists(addressed_path(*filesystem, input_path), stop_token));
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
    result.source_path = absolute_display_path(*filesystem, input_path);
    auto content = co_await std::move(filesystem->readTextFile(addressed_path(*filesystem, input_path), stop_token));
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

    if (!filesystems.workspace) {
        co_return std::unexpected(harness::FileError{
                .code = harness::FileErrorCode::Invalid,
                .message = "project resource loading requires a workspace filesystem",
                .path = std::nullopt,
        });
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
    PromptNameTracker prompt_names;
    for (const auto& input : request.explicit_prompt_templates) {
        const auto input_path = std::filesystem::path{input.path};
        const auto addressed_input = input_path.is_absolute() ? input_path : workspace / input_path;
        auto filesystem = explicit_resource_filesystem_for(filesystems, addressed_input);
        if (!filesystem) {
            const auto unauthorized = unauthorized_resource_path(addressed_input);
            result.fatal_errors.push_back(error_diagnostic(unauthorized.message, input.path));
            continue;
        }
        auto loaded = co_await std::move(loadPromptTemplates(*filesystem,
                std::vector<PromptTemplateDirSpec>{PromptTemplateDirSpec{
                        .path = addressed_path(*filesystem, addressed_input),
                        .is_file = input.is_file,
                        .source_info =
                                SourceInfo{
                                        .path = {},
                                        .source = "cli",
                                        .scope = SourceScope::Temporary,
                                        .origin = SourceOrigin::TopLevel,
                                        .base_dir = std::nullopt,
                                },
                }},
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
                std::vector<PromptTemplateDirSpec>{PromptTemplateDirSpec{
                        .path = ".pi/prompts",
                        .is_file = false,
                        .source_info =
                                SourceInfo{
                                        .path = {},
                                        .source = "auto",
                                        .scope = SourceScope::Project,
                                        .origin = SourceOrigin::TopLevel,
                                        .base_dir = (workspace / ".pi" / "prompts").string(),
                                },
                }},
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
                std::vector<PromptTemplateDirSpec>{PromptTemplateDirSpec{
                        .path = "prompts",
                        .is_file = false,
                        .source_info =
                                SourceInfo{
                                        .path = {},
                                        .source = "auto",
                                        .scope = SourceScope::User,
                                        .origin = SourceOrigin::TopLevel,
                                        .base_dir = normalized_absolute(*request.agent_config_directory).string(),
                                },
                }},
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

    // Skills use one name tracker across all authorized roots, so first-wins
    // precedence and collision diagnostics remain independent of adapters.
    SkillNameTracker skill_names;
    if (!request.no_skills) {
        if (project_trusted) {
            auto loaded = co_await std::move(loadSkills(*filesystems.workspace,
                    std::vector<SkillDirSpec>{SkillDirSpec{
                            .path = ".pi/skills",
                            .include_root_files = true,
                            .source_context =
                                    SkillSourceContext{
                                            .source = "auto",
                                            .scope = SourceScope::Project,
                                            .base_dir = (workspace / ".pi").string(),
                                    },
                    }},
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
                        std::vector<SkillDirSpec>{SkillDirSpec{
                                .path = ".agents/skills",
                                .include_root_files = false,
                                .source_context =
                                        SkillSourceContext{
                                                .source = "auto",
                                                .scope = SourceScope::Project,
                                                .base_dir = (root / ".agents").string(),
                                        },
                        }},
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
                    std::vector<SkillDirSpec>{SkillDirSpec{
                            .path = "skills",
                            .include_root_files = true,
                            .source_context =
                                    SkillSourceContext{
                                            .source = "auto",
                                            .scope = SourceScope::User,
                                            .base_dir = normalized_absolute(*request.agent_config_directory).string(),
                                    },
                    }},
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
                    std::vector<SkillDirSpec>{SkillDirSpec{
                            .path = "skills",
                            .include_root_files = false,
                            .source_context =
                                    SkillSourceContext{
                                            .source = "auto",
                                            .scope = SourceScope::User,
                                            .base_dir = normalized_absolute(filesystems.user_agents_root->workspace())
                                                    .string(),
                                    },
                    }},
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
    for (const auto& path : request.skill_paths) {
        const auto skill_path = std::filesystem::path{path};
        const auto addressed_skill_path = skill_path.is_absolute() ? skill_path : workspace / skill_path;
        auto filesystem = explicit_resource_filesystem_for(filesystems, addressed_skill_path);
        if (!filesystem) {
            skill_sink.push(warning_diagnostic(unauthorized_resource_path(addressed_skill_path).message, path));
            continue;
        }
        auto info =
                co_await std::move(filesystem->fileInfo(addressed_path(*filesystem, addressed_skill_path), stop_token));
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
        auto loaded = co_await std::move(loadSkills(*filesystem,
                std::vector<SkillDirSpec>{SkillDirSpec{
                        .path = path,
                        .include_root_files = true,
                        .source_context =
                                SkillSourceContext{
                                        .source = "cli",
                                        .scope = SourceScope::Temporary,
                                        .base_dir = std::nullopt,
                                },
                }},
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
        auto listed = co_await std::move(filesystems.workspace->listDir(".pi/themes", stop_token));
        if (!listed) {
            if (loader_aborted(listed.error())) {
                co_return std::unexpected(std::move(listed.error()));
            }
            if (listed.error().code != harness::FileErrorCode::NotFound) {
                theme_sink.push(warning_diagnostic(listed.error().message, ".pi/themes"));
            }
        } else {
            std::sort(listed->begin(), listed->end(), [](const auto& left, const auto& right) {
                return left.name < right.name;
            });
            for (const auto& entry : *listed) {
                if (entry.kind != harness::FileKind::File || !entry.name.ends_with(".json")) {
                    continue;
                }
                const auto path = std::string{".pi/themes/"} + entry.name;
                auto content = co_await std::move(filesystems.workspace->readTextFile(path, stop_token));
                if (!content) {
                    if (loader_aborted(content.error())) {
                        co_return std::unexpected(std::move(content.error()));
                    }
                    theme_sink.push(warning_diagnostic(content.error().message, path));
                } else {
                    result.resources.themes.push_back({
                            .path = path,
                            .json = std::move(*content),
                            .scope = SourceScope::Project,
                    });
                }
            }
        }
    }
    if (!request.no_themes && filesystems.agent_config_directory && request.agent_config_directory &&
            !request.agent_config_directory->empty()) {
        auto listed = co_await std::move(filesystems.agent_config_directory->listDir("themes", stop_token));
        if (!listed) {
            if (loader_aborted(listed.error())) {
                co_return std::unexpected(std::move(listed.error()));
            }
            if (listed.error().code != harness::FileErrorCode::NotFound) {
                theme_sink.push(warning_diagnostic(listed.error().message,
                        normalized_absolute(*request.agent_config_directory / "themes").string()));
            }
        } else {
            std::sort(listed->begin(), listed->end(), [](const auto& left, const auto& right) {
                return left.name < right.name;
            });
            for (const auto& entry : *listed) {
                if (entry.kind != harness::FileKind::File || !entry.name.ends_with(".json")) {
                    continue;
                }
                const auto relative = std::string{"themes/"} + entry.name;
                const auto display = normalized_absolute(*request.agent_config_directory / relative).string();
                auto content =
                        co_await std::move(filesystems.agent_config_directory->readTextFile(relative, stop_token));
                if (!content) {
                    if (loader_aborted(content.error())) {
                        co_return std::unexpected(std::move(content.error()));
                    }
                    theme_sink.push(warning_diagnostic(content.error().message, display));
                } else {
                    result.resources.themes.push_back({
                            .path = display,
                            .json = std::move(*content),
                            .scope = SourceScope::User,
                    });
                }
            }
        }
    }
    for (const auto& path : request.theme_paths) {
        const auto theme_path = std::filesystem::path{path};
        const auto addressed_theme_path = theme_path.is_absolute() ? theme_path : workspace / theme_path;
        auto filesystem = explicit_resource_filesystem_for(filesystems, addressed_theme_path);
        if (!filesystem) {
            theme_sink.push(warning_diagnostic(unauthorized_resource_path(addressed_theme_path).message, path));
            continue;
        }
        auto info =
                co_await std::move(filesystem->fileInfo(addressed_path(*filesystem, addressed_theme_path), stop_token));
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
            auto listed = co_await std::move(filesystem->listDir(addressed_path(*filesystem, path), stop_token));
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
                    files.push_back(path + "/" + entry.name);
                }
            }
        } else if (info->kind == harness::FileKind::File) {
            if (!path.ends_with(".json")) {
                theme_sink.push(warning_diagnostic("theme path is not a json file", path));
                continue;
            }
            files.push_back(path);
        } else {
            theme_sink.push(warning_diagnostic("theme path is not a json file", path));
            continue;
        }
        for (const auto& file : files) {
            auto content = co_await std::move(filesystem->readTextFile(addressed_path(*filesystem, file), stop_token));
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
    bool system_source_discovered = false;
    if (!system_source && project_trusted) {
        auto discovered = co_await std::move(to_async_result(
                find_existing_path_task(filesystems.workspace, workspace / ".pi/SYSTEM.md", stop_token)));
        if (!discovered) co_return std::unexpected(std::move(discovered.error()));
        if (*discovered) {
            system_source = std::move(**discovered);
            system_source_discovered = true;
        }
    }
    if (!system_source && filesystems.agent_config_directory && request.agent_config_directory &&
            !request.agent_config_directory->empty()) {
        auto discovered = co_await std::move(to_async_result(find_existing_path_task(
                filesystems.agent_config_directory, *request.agent_config_directory / "SYSTEM.md", stop_token)));
        if (!discovered) co_return std::unexpected(std::move(discovered.error()));
        if (*discovered) {
            system_source = std::move(**discovered);
            system_source_discovered = true;
        }
    }
    auto resolved_system = co_await std::move(to_async_result(resolve_prompt_input_task(
            filesystems,
            std::move(system_source),
            "system prompt",
            diagnostics,
            system_source_discovered,
            stop_token)));
    if (!resolved_system) co_return std::unexpected(std::move(resolved_system.error()));
    result.resources.system_prompt = std::move(resolved_system->text);
    result.resources.system_prompt_source = std::move(resolved_system->source_path);

    std::vector<std::string> append_sources;
    bool append_sources_discovered = false;
    if (!request.append_system_prompt.empty()) {
        append_sources = request.append_system_prompt;
    } else {
        if (project_trusted) {
            auto discovered = co_await std::move(to_async_result(
                    find_existing_path_task(filesystems.workspace, workspace / ".pi/APPEND_SYSTEM.md", stop_token)));
            if (!discovered) co_return std::unexpected(std::move(discovered.error()));
            if (*discovered) {
                append_sources.push_back(std::move(**discovered));
                append_sources_discovered = true;
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
                append_sources_discovered = true;
            }
        }
    }
    for (auto& source : append_sources) {
        auto resolved = co_await std::move(to_async_result(resolve_prompt_input_task(
                filesystems,
                std::optional<std::string>{source},
                "append system prompt",
                diagnostics,
                append_sources_discovered,
                stop_token)));
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

ProjectResourceLoadingResult load_project_resources(const harness::WorkspaceFileSystem& filesystem,
        const ProjectTrustStore& trust_store,
        ProjectResourceLoadingRequest request) {
    auto operation =
            load_project_resources(make_compatibility_filesystems(filesystem, request), trust_store, request, {});
    std::optional<std::expected<ProjectResourceLoadingResult, harness::FileError>> outcome;
    std::move(operation).start(
            [&outcome](std::expected<ProjectResourceLoadingResult, harness::FileError> value) noexcept {
                outcome.emplace(std::move(value));
            });
    if (outcome && *outcome) {
        return std::move(**outcome);
    }
    ProjectResourceLoadingResult failure;
    if (outcome) {
        failure.diagnostics.push_back(error_diagnostic(outcome->error().message, outcome->error().path));
    } else {
        failure.diagnostics.push_back(
                error_diagnostic("synchronous project resource compatibility bridge did not complete"));
    }
    return failure;
}

} // namespace cch::coding_agent

#include "coding_agent/ProjectResourceLoader.hpp"

#include <cch/coding_agent/AgentConfigDir.hpp>
#include <cch/support/Error.hpp>

#include "coding_agent/PromptTemplateLoader.hpp"
#include "coding_agent/ResourceDiagnosticPolicy.hpp"
#include "coding_agent/SkillLoader.hpp"
#include "agent/harness/WorkspaceFileSystem.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace cch::coding_agent {
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

/// Raw text read for pi `readFileSync(path, "utf-8")` on the loader's
/// out-of-workspace paths (the ancestor chain and the Agent Config
/// Directory). A non-regular entry fails like pi's `readFileSync` throw on
/// a directory.
[[nodiscard]] support::Expected<std::string> read_text_file(
    const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec) || ec) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Unknown,
            "could not read " + path.string()));
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Unknown,
            "could not read " + path.string()));
    }
    std::ostringstream content;
    content << input.rdbuf();
    if (input.bad()) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Unknown,
            "could not read " + path.string()));
    }
    return content.str();
}

/// pi `canonicalizePath` (`utils/paths.ts`): `realpathSync` with the input
/// path as the fallback when the real path cannot be resolved.
[[nodiscard]] std::filesystem::path canonicalize_path(
    const std::filesystem::path& path) {
    std::error_code ec;
    auto canonical = std::filesystem::canonical(path, ec);
    return ec ? path : canonical;
}

/// pi `loadContextFileFromDir` (`resource-loader.ts`): the first existing
/// regular file among the AGENTS.md/AGENTS.MD/CLAUDE.md/CLAUDE.MD
/// candidates. A read failure warns (pi `console.error(chalk.yellow(...))`)
/// and moves to the next candidate.
[[nodiscard]] std::optional<prompt::ProjectContextFile> load_context_file_from_dir(
    const std::filesystem::path& dir,
    std::vector<ResourceDiagnostic>& diagnostics) {
    for (const auto& filename : kContextFileCandidates) {
        const auto file_path = dir / filename;
        std::error_code ec;
        if (!std::filesystem::exists(file_path, ec) || ec) {
            continue;
        }
        std::error_code file_ec;
        if (!std::filesystem::is_regular_file(file_path, file_ec) || file_ec) {
            // pi `!statSync(filePath).isFile() → continue`.
            continue;
        }
        auto content = read_text_file(file_path);
        if (!content) {
            diagnostics.push_back(warning_diagnostic(
                "Could not read " + file_path.string() + ": " +
                    content.error().message,
                file_path.string()));
            continue;
        }
        return prompt::ProjectContextFile{
            .path = file_path.string(),
            .content = std::move(*content),
        };
    }
    return std::nullopt;
}

/// pi `findGitPaths` (`footer-data-provider.ts`): walk up from `start`; the
/// first directory with a `.git` entry — a linked-worktree pointer file
/// whose `gitdir:` target carries `HEAD`, or a plain directory with `HEAD`
/// — yields the repo root and the common git directory. A `.git` entry that
/// fails its check stops the walk (pi returns null); an entry that is not a
/// file or directory is skipped.
struct GitPaths {
    std::filesystem::path repo_dir;
    std::filesystem::path common_git_dir;
};

[[nodiscard]] std::optional<GitPaths> find_git_paths(
    const std::filesystem::path& start) {
    std::error_code ec;
    auto dir = std::filesystem::absolute(start, ec);
    if (ec) {
        return std::nullopt;
    }
    dir = dir.lexically_normal();
    while (true) {
        const auto git_path = dir / ".git";
        std::error_code exists_ec;
        if (std::filesystem::exists(git_path, exists_ec) && !exists_ec) {
            std::error_code stat_ec;
            const auto status = std::filesystem::status(git_path, stat_ec);
            // pi: `statSync` inside the try — an entry that exists but
            // cannot be statted stops the walk (returns null).
            if (stat_ec) {
                return std::nullopt;
            }
            if (std::filesystem::is_regular_file(status)) {
                auto content = read_text_file(git_path);
                // pi: `readFileSync` throws → catch → null.
                if (!content) {
                    return std::nullopt;
                }
                const auto trimmed = trim(*content);
                if (trimmed.starts_with("gitdir: ")) {
                    // pi `resolve(dir, content.slice(8).trim())`.
                    const auto git_dir =
                        (dir / trimmed.substr(8)).lexically_normal();
                    const auto head_path = git_dir / "HEAD";
                    if (!std::filesystem::exists(head_path, ec) || ec) {
                        return std::nullopt;
                    }
                    std::filesystem::path common_git_dir = git_dir;
                    const auto commondir_path = git_dir / "commondir";
                    if (std::filesystem::exists(commondir_path, ec) &&
                        !ec) {
                        auto commondir = read_text_file(commondir_path);
                        // pi: a commondir read failure also throws inside
                        // the try → null.
                        if (!commondir) {
                            return std::nullopt;
                        }
                        // pi `resolve(gitDir, ...)`.
                        common_git_dir =
                            (git_dir /
                             std::filesystem::path{trim(*commondir)})
                                .lexically_normal();
                    }
                    return GitPaths{
                        .repo_dir = dir,
                        .common_git_dir = std::move(common_git_dir),
                    };
                }
            } else if (std::filesystem::is_directory(status)) {
                const auto head_path = git_path / "HEAD";
                if (!std::filesystem::exists(head_path, ec) || ec) {
                    return std::nullopt;
                }
                return GitPaths{
                    .repo_dir = dir,
                    .common_git_dir = git_path,
                };
            }
        }
        const auto parent = dir.parent_path();
        if (parent == dir) {
            return std::nullopt;
        }
        dir = parent;
    }
}

/// pi `findShadowedContextFile` (`resource-loader.ts`): the main repo's
/// context file that a nested linked worktree's own copy shadows — both are
/// the same tracked AGENTS.md/CLAUDE.md, so loading both loads it twice.
/// Returns nullopt when nothing is shadowed, leaving normal ancestor
/// inheritance alone. Both compared paths are canonicalized (realpath),
/// because `git worktree add` writes the `.git` file's `gitdir:` target in
/// realpath form while cwd may still be symlinked.
[[nodiscard]] std::optional<std::filesystem::path> find_shadowed_context_file(
    const std::filesystem::path& cwd,
    std::vector<ResourceDiagnostic>& diagnostics) {
    const auto git_paths = find_git_paths(cwd);
    if (!git_paths) {
        return std::nullopt;
    }
    const auto common_git_dir = canonicalize_path(git_paths->common_git_dir);
    const auto worktree_root = canonicalize_path(git_paths->repo_dir);
    const auto main_repo_root = common_git_dir.parent_path();
    // False for an ordinary repo, where the two are the same dir, and for a
    // sibling worktree (`git worktree add ../feat`), whose main repo is not
    // an ancestor.
    // `string + char` appends the separator; `std::string{1, sep}` would
    // pick the initializer-list constructor (two chars), not a run.
    const std::string main_repo_prefix =
        main_repo_root.string() +
        std::filesystem::path::preferred_separator;
    if (!worktree_root.string().starts_with(main_repo_prefix)) {
        return std::nullopt;
    }
    // dirname of the common git dir is the main worktree root only when
    // that dir is itself checked out from the same repo. In a bare layout
    // (`proj/.bare` + `proj/main`) it is just the directory holding `.bare`,
    // which tracks nothing; a submodule's gitdir has no `commondir`, so it
    // lands under `.git/modules`.
    if (canonicalize_path(main_repo_root / ".git") != common_git_dir) {
        return std::nullopt;
    }
    const auto worktree_context_file =
        load_context_file_from_dir(worktree_root, diagnostics);
    if (!worktree_context_file) {
        return std::nullopt;
    }
    return main_repo_root / std::filesystem::path{worktree_context_file->path}.filename();
}

/// pi `loadProjectContextFiles` (`resource-loader.ts`): the global context
/// file from the Agent Config Directory, then the cwd ancestor chain
/// root-most first (the walk unshifts each found file), with linked-worktree
/// shadowing and raw-path dedupe. Not Project Trust gated.
[[nodiscard]] std::vector<prompt::ProjectContextFile> load_project_context_files(
    const std::filesystem::path& cwd,
    const std::optional<std::filesystem::path>& agent_config_directory,
    bool no_context_files,
    std::vector<ResourceDiagnostic>& diagnostics) {
    if (no_context_files) {
        return {};
    }
    // pi `resolvePath` (lexical normalization, no symlink resolution).
    const auto resolved_cwd =
        std::filesystem::absolute(cwd).lexically_normal();

    std::vector<prompt::ProjectContextFile> context_files;
    std::set<std::string> seen_paths;

    if (agent_config_directory && !agent_config_directory->empty()) {
        const auto resolved_agent_dir =
            std::filesystem::absolute(*agent_config_directory)
                .lexically_normal();
        if (auto global =
                load_context_file_from_dir(resolved_agent_dir, diagnostics)) {
            context_files.push_back(std::move(*global));
            seen_paths.insert(context_files.back().path);
        }
    }

    const auto shadowed_context_file =
        find_shadowed_context_file(resolved_cwd, diagnostics);

    std::vector<prompt::ProjectContextFile> ancestor_context_files;
    auto current_dir = resolved_cwd;
    while (true) {
        auto context_file =
            load_context_file_from_dir(current_dir, diagnostics);
        // pi: the shadow check compares canonicalized paths against the
        // (already canonical) main-repo copy.
        const bool is_shadowed =
            shadowed_context_file.has_value() &&
            canonicalize_path(
                context_file ? std::filesystem::path{context_file->path}
                             : std::filesystem::path{})
                    .string() == shadowed_context_file->string();
        if (context_file && !is_shadowed &&
            !seen_paths.contains(context_file->path)) {
            const auto path = context_file->path;
            // pi `ancestorContextFiles.unshift(...)`: the walk runs cwd-first,
            // so the final order is root-most first, cwd last.
            ancestor_context_files.insert(
                ancestor_context_files.begin(), std::move(*context_file));
            seen_paths.insert(path);
        }
        const auto parent = current_dir.parent_path();
        if (parent == current_dir) {
            break;
        }
        current_dir = parent;
    }
    context_files.insert(
        context_files.end(),
        std::make_move_iterator(ancestor_context_files.begin()),
        std::make_move_iterator(ancestor_context_files.end()));
    return context_files;
}

/// pi `resolvePromptInput` (`resource-loader.ts`): a value naming an
/// existing file is read as text — a read failure warns (pi
/// `console.error(chalk.yellow("Warning: ..."))`) and falls back to the raw
/// value; anything else is the prompt text itself. An absent or empty value
/// resolves to no prompt.
[[nodiscard]] std::optional<std::string> resolve_prompt_input(
    const std::optional<std::string>& input,
    std::string_view description,
    std::vector<ResourceDiagnostic>& diagnostics) {
    if (!input || input->empty()) {
        return std::nullopt;
    }
    std::error_code ec;
    if (std::filesystem::exists(*input, ec) && !ec) {
        auto content = read_text_file(*input);
        if (!content) {
            diagnostics.push_back(warning_diagnostic(
                "Could not read " + std::string{description} + " file " +
                    *input + ": " + content.error().message,
                *input));
            return input;
        }
        return *content;
    }
    return input;
}

/// pi `discoverSystemPromptFile` (`resource-loader.ts`): the project
/// `.pi/SYSTEM.md` (Project Trust gated), then the global
/// `<agent_config_directory>/SYSTEM.md`.
[[nodiscard]] std::optional<std::filesystem::path> discover_system_prompt_file(
    const std::filesystem::path& workspace,
    const std::optional<std::filesystem::path>& agent_config_directory,
    bool project_trusted) {
    const auto project_path = workspace / ".pi" / "SYSTEM.md";
    if (project_trusted && std::filesystem::exists(project_path)) {
        return project_path;
    }
    if (agent_config_directory && !agent_config_directory->empty()) {
        const auto global_path = *agent_config_directory / "SYSTEM.md";
        if (std::filesystem::exists(global_path)) {
            return global_path;
        }
    }
    return std::nullopt;
}

/// pi `discoverAppendSystemPromptFile` (`resource-loader.ts`): the project
/// `.pi/APPEND_SYSTEM.md` (Project Trust gated), then the global
/// `<agent_config_directory>/APPEND_SYSTEM.md`.
[[nodiscard]] std::optional<std::filesystem::path> discover_append_system_prompt_file(
    const std::filesystem::path& workspace,
    const std::optional<std::filesystem::path>& agent_config_directory,
    bool project_trusted) {
    const auto project_path = workspace / ".pi" / "APPEND_SYSTEM.md";
    if (project_trusted && std::filesystem::exists(project_path)) {
        return project_path;
    }
    if (agent_config_directory && !agent_config_directory->empty()) {
        const auto global_path = *agent_config_directory / "APPEND_SYSTEM.md";
        if (std::filesystem::exists(global_path)) {
            return global_path;
        }
    }
    return std::nullopt;
}

/// pi `systemPromptSourcePath`: the resolved SYSTEM.md source path when the
/// resolved custom prompt came from a file (the loaded-resources Context
/// presentation reads it).
[[nodiscard]] std::optional<std::string> resolved_source_path(
    const std::optional<std::string>& source) {
    if (!source) {
        return std::nullopt;
    }
    std::error_code ec;
    if (!std::filesystem::exists(*source, ec) || ec) {
        return std::nullopt;
    }
    return std::filesystem::absolute(*source).lexically_normal().string();
}

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
void load_user_skills_adapter(
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
void load_project_agents_skills_adapter(
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
void load_user_agents_skills_adapter(
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
void load_explicit_skill_paths(
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


void load_project_prompts_adapter(
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

void load_user_prompts_adapter(
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
void load_project_themes_adapter(
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
void load_user_themes_adapter(
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
void load_explicit_theme_paths(
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
void load_explicit_prompt_template_input(
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

ProjectResourceLoadingResult load_project_resources(
    const harness::WorkspaceFileSystem& fs,
    const ProjectTrustStore& trust_store,
    ProjectResourceLoadingRequest request) {
    ProjectResourceLoadingResult result;
    // The merged diagnostics keep pi's loader order (trust → prompts →
    // skills → themes → context → system); the per-kind sinks additionally
    // bucket the prompt/skill/theme diagnostics for the loaded-resources
    // presentation (#418).
    std::vector<ResourceDiagnostic> diagnostics;
    std::vector<ResourceDiagnostic> prompt_diagnostics;
    std::vector<ResourceDiagnostic> skill_diagnostics;
    std::vector<ResourceDiagnostic> theme_diagnostics;
    KindedDiagnosticSink prompt_sink{diagnostics, prompt_diagnostics};
    KindedDiagnosticSink skill_sink{diagnostics, skill_diagnostics};
    KindedDiagnosticSink theme_sink{diagnostics, theme_diagnostics};

    const auto workspace = request.workspace.empty() ? fs.root() : request.workspace;
    const auto home = request.home_directory.value_or(home_directory());
    const auto user_agents_skills_dir = home / ".agents" / "skills";

    // 1. Detect `.pi/` and `.agents/skills` markers. The mere presence of
    // any loadable marker triggers the Project Trust decision; the decision
    // then gates all project resource loading ("trusted means load").
    result.detection = detect_project_resources(fs, user_agents_skills_dir);
    const bool trust_needed = needs_project_trust_resolution(result.detection);
    result.trust = resolve_project_trust(
        workspace,
        trust_needed,
        trust_store,
        request.default_project_trust,
        request.project_trust_override);
    add_trust_diagnostics(diagnostics, result.trust);

    const bool project_trusted = result.trust.decision == ProjectTrustDecision::Trusted;

    // 2. Prompt templates load in pi's order — explicit `--prompt-template`
    // inputs, then the trust-gated project `.pi/prompts`, then user
    // `<agent_config_directory>/prompts` — with first-wins dedupe and
    // collision diagnostics. `--no-prompt-templates` drops user and project
    // discovery but keeps explicit inputs.
    PromptNameTracker names;
    for (auto& input : request.explicit_prompt_templates) {
        load_explicit_prompt_template_input(fs, result, prompt_sink, names, std::move(input));
    }
    if (!request.no_prompt_templates && project_trusted) {
        load_project_prompts_adapter(fs, result, prompt_sink, names);
    }
    if (!request.no_prompt_templates && request.agent_config_directory &&
        !request.agent_config_directory->empty()) {
        load_user_prompts_adapter(*request.agent_config_directory, result, prompt_sink, names);
    }

    // 3. Skills load in pi's order (resource-loader.ts `mergePaths` puts the
    // auto-discovered paths before the `--skill` additions, and `loadSkills`
    // `addSkills` is first-wins): trust-gated project `.pi/skills` (pi
    // discovery mode), the trust-gated project `.agents/skills` ancestors,
    // user `~/.pi/agent/skills`, user `~/.agents/skills`, and the explicit
    // `--skill` paths LAST (discovered skills win name collisions).
    // `--no-skills` drops every discovery source but keeps the explicit
    // paths. One name tracker spans every source like pi's single
    // `loadSkills` call.
    SkillNameTracker skill_names;
    if (!request.no_skills) {
        if (project_trusted) {
            load_skills_adapter(fs, result, skill_sink, skill_names, SkillDirSpec{
                .path = std::string{kProjectSkillsMarker},
                .include_root_files = true,
                .source_context = SkillSourceContext{
                    .source = "auto",
                    .scope = SourceScope::Project,
                    .base_dir = (workspace / ".pi").string(),
                },
            });
        }
        load_project_agents_skills_adapter(
            fs,
            workspace,
            user_agents_skills_dir,
            project_trusted,
            result,
            skill_sink,
            skill_names);
        if (request.agent_config_directory && !request.agent_config_directory->empty()) {
            load_user_skills_adapter(*request.agent_config_directory, result, skill_sink, skill_names);
        }
        load_user_agents_skills_adapter(user_agents_skills_dir, result, skill_sink, skill_names);
    }
    load_explicit_skill_paths(fs, request.skill_paths, result, skill_sink, skill_names);

    // 4. Themes load in pi's order (resource-loader.ts `mergePaths` puts
    // the auto-discovered paths before the `--theme` additions, and
    // `dedupeThemes` is first-wins): trust-gated project `.pi/themes`, user
    // `<agent_config_directory>/themes`, then the explicit `--theme` paths
    // LAST (discovered themes win name collisions). `--no-themes` drops
    // every discovery source but keeps the explicit paths. Parsing and
    // name-level dedupe stay in the coding-agent TUI package
    // (`discover_themes`), which receives the collected documents.
    if (!request.no_themes && project_trusted) {
        load_project_themes_adapter(fs, result, theme_sink);
    }
    if (!request.no_themes && request.agent_config_directory &&
        !request.agent_config_directory->empty()) {
        load_user_themes_adapter(*request.agent_config_directory, result, theme_sink);
    }
    load_explicit_theme_paths(fs, request.theme_paths, result, theme_sink);

    // 5. Project Context Files (pi `loadProjectContextFiles`): the global
    // context file from the Agent Config Directory plus the cwd ancestor
    // chain with linked-worktree shadowing. NOT gated by Project Trust
    // (pinned fact); `--no-context-files` disables discovery. The files
    // render into the System Prompt as `<project_context>`/
    // `<project_instructions path="...">`.
    result.resources.agents_files = load_project_context_files(
        workspace,
        request.agent_config_directory,
        request.no_context_files,
        diagnostics);

    // 6. SYSTEM.md / APPEND_SYSTEM.md (pi `discoverSystemPromptFile` /
    // `discoverAppendSystemPromptFile`): the project `.pi/` files are
    // Project Trust gated (their mere presence already triggers the trust
    // decision through the markers above), then the global Agent Config
    // Directory. The CLI `--system-prompt`/`--append-system-prompt` values
    // win over discovery (pi `systemPromptSource ?? discovery`); each source
    // resolves through pi's `resolvePromptInput` text-or-file semantics.
    std::optional<std::string> system_prompt_source = request.system_prompt;
    if (!system_prompt_source) {
        if (auto discovered = discover_system_prompt_file(
                workspace,
                request.agent_config_directory,
                project_trusted)) {
            system_prompt_source = discovered->string();
        }
    }
    result.resources.system_prompt =
        resolve_prompt_input(system_prompt_source, "system prompt", diagnostics);
    result.resources.system_prompt_source =
        resolved_source_path(system_prompt_source);

    std::vector<std::string> append_sources;
    if (!request.append_system_prompt.empty()) {
        append_sources = request.append_system_prompt;
    } else if (auto discovered = discover_append_system_prompt_file(
                   workspace,
                   request.agent_config_directory,
                   project_trusted)) {
        append_sources.push_back(discovered->string());
    }
    for (const auto& source : append_sources) {
        if (auto resolved = resolve_prompt_input(
                std::optional<std::string>{source},
                "append system prompt",
                diagnostics)) {
            result.resources.append_system_prompt.push_back(std::move(*resolved));
        }
        if (auto source_path = resolved_source_path(
                std::optional<std::string>{source})) {
            result.resources.append_system_prompt_sources.push_back(
                std::move(*source_path));
        }
    }

    result.diagnostics = std::move(diagnostics);
    result.skill_diagnostics = std::move(skill_diagnostics);
    result.prompt_diagnostics = std::move(prompt_diagnostics);
    result.theme_diagnostics = std::move(theme_diagnostics);
    bound_diagnostics(result);
    return result;
}

} // namespace cch::coding_agent

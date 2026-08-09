#include "coding_agent/ProjectResourceLoader.hpp"

#include <cch/coding_agent/AgentConfigDir.hpp>

#include "coding_agent/PromptTemplateLoader.hpp"
#include "coding_agent/ResourceDiagnosticPolicy.hpp"
#include "coding_agent/SkillLoader.hpp"
#include "../harness/WorkspaceFileSystem.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <optional>
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
        std::vector<ResourceDiagnostic>& diagnostics,
        std::vector<Skill> incoming) {
        for (auto& skill : incoming) {
            if (auto it = by_name_.find(skill.name); it != by_name_.end()) {
                diagnostics.push_back(skill_collision_diagnostic(
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
        std::vector<ResourceDiagnostic>& diagnostics,
        std::vector<PromptTemplate> incoming) {
        for (auto& tmpl : incoming) {
            if (auto it = by_name_.find(tmpl.name); it != by_name_.end()) {
                diagnostics.push_back(prompt_collision_diagnostic(ResourceCollision{
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
    std::vector<ResourceDiagnostic>& diagnostics,
    SkillNameTracker& names,
    SkillDirSpec spec) {
    auto load = loadSkills(fs, {std::move(spec)});
    for (auto& diagnostic : load.diagnostics) {
        auto type = diagnostic.type == "collision"
            ? ResourceDiagnosticType::Collision
            : ResourceDiagnosticType::Warning;
        diagnostics.push_back(ResourceDiagnostic{
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
    names.append(result.resources.skills, diagnostics, std::move(load.skills));
}

/// User skills from `<agent_config_directory>/skills` — pi discovery mode
/// (`loadSkillsFromDirInternal(..., "user", true)`): root-level `.md` files
/// included, sourceInfo scope user with the agent dir as baseDir. Not
/// trust-gated. A missing agent config directory loads nothing silently.
void load_user_skills_adapter(
    const std::filesystem::path& agent_config_directory,
    ProjectResourceLoadingResult& result,
    std::vector<ResourceDiagnostic>& diagnostics,
    SkillNameTracker& names) {
    auto fs = harness::WorkspaceFileSystem::create(agent_config_directory);
    if (!fs) {
        return;
    }
    load_skills_adapter(*fs, result, diagnostics, names, SkillDirSpec{
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
    const harness::WorkspaceFileSystem& fs,
    const std::filesystem::path& workspace,
    const std::filesystem::path& user_agents_skills_dir,
    bool project_trusted,
    ProjectResourceLoadingResult& result,
    std::vector<ResourceDiagnostic>& diagnostics,
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
                load_skills_adapter(*ancestor_fs, result, diagnostics, names, SkillDirSpec{
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
    std::vector<ResourceDiagnostic>& diagnostics,
    SkillNameTracker& names) {
    if (auto user_fs = harness::WorkspaceFileSystem::create(
            user_agents_skills_dir.parent_path());
        user_fs) {
        load_skills_adapter(*user_fs, result, diagnostics, names, SkillDirSpec{
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
    std::vector<ResourceDiagnostic>& diagnostics,
    SkillNameTracker& names) {
    for (const auto& path : skill_paths) {
        const auto info = fs.fileInfo(path);
        if (!info) {
            if (info.error().code == harness::FileErrorCode::NotFound) {
                diagnostics.push_back(warning_diagnostic(
                    "skill path does not exist",
                    path));
                diagnostics.push_back(error_diagnostic(
                    "Skill path does not exist",
                    path));
            } else {
                diagnostics.push_back(warning_diagnostic(
                    info.error().message,
                    path));
            }
            continue;
        }
        load_skills_adapter(fs, result, diagnostics, names, SkillDirSpec{
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
    std::vector<ResourceDiagnostic>& diagnostics,
    PromptNameTracker& names) {
    auto load = loadPromptTemplates(
        fs,
        {PromptTemplateDirSpec{.path = std::string{kProjectPromptsMarker}, .is_file = false}});
    for (auto& diagnostic : load.diagnostics) {
        diagnostics.push_back(warning_diagnostic(
            std::move(diagnostic.message),
            std::move(diagnostic.path)));
    }
    names.append(result.resources.prompt_templates, diagnostics, std::move(load.templates));
}

void load_user_prompts_adapter(
    const std::filesystem::path& agent_config_directory,
    ProjectResourceLoadingResult& result,
    std::vector<ResourceDiagnostic>& diagnostics,
    PromptNameTracker& names) {
    auto fs = harness::WorkspaceFileSystem::create(agent_config_directory);
    if (!fs) {
        // The Agent Config Directory does not exist (or is unreadable); a
        // fresh setup has no user prompts. Best-effort by contract.
        return;
    }
    auto load = loadPromptTemplates(
        *fs,
        {PromptTemplateDirSpec{.path = "prompts", .is_file = false}});
    for (auto& diagnostic : load.diagnostics) {
        diagnostics.push_back(warning_diagnostic(
            std::move(diagnostic.message),
            (agent_config_directory / diagnostic.path).string()));
    }
    names.append(result.resources.prompt_templates, diagnostics, std::move(load.templates));
}

void load_project_themes_adapter(
    const harness::WorkspaceFileSystem& fs,
    ProjectResourceLoadingResult& result,
    std::vector<ResourceDiagnostic>& diagnostics) {
    if (auto listed = fs.listDir(std::string{kProjectThemesMarker}); !listed) {
        diagnostics.push_back(warning_diagnostic(
            listed.error().message,
            std::string{kProjectThemesMarker}));
    } else {
        std::sort(listed->begin(), listed->end(), [](const auto& left, const auto& right) {
            return left.name < right.name;
        });
        for (const auto& entry : *listed) {
            if (entry.kind != harness::FileKind::File || !entry.name.ends_with(".json")) continue;
            const auto path = std::string{kProjectThemesMarker} + "/" + entry.name;
            if (auto content = fs.readTextFile(path); !content) {
                diagnostics.push_back(warning_diagnostic(
                    content.error().message,
                    path));
            } else {
                result.resources.project_themes.push_back({
                    .path = path,
                    .json = std::move(*content),
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
    std::vector<ResourceDiagnostic>& diagnostics,
    PromptNameTracker& names,
    ExplicitPromptTemplateInput input) {
    auto input_path = std::move(input.path);
    auto explicit_load = loadPromptTemplates(
        fs,
        {PromptTemplateDirSpec{
            .path = input_path,
            .is_file = input.is_file,
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

    names.append(result.resources.prompt_templates, diagnostics, std::move(explicit_load.templates));
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
    std::vector<ResourceDiagnostic> diagnostics;

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
        load_explicit_prompt_template_input(fs, result, diagnostics, names, std::move(input));
    }
    if (!request.no_prompt_templates && project_trusted) {
        load_project_prompts_adapter(fs, result, diagnostics, names);
    }
    if (!request.no_prompt_templates && request.agent_config_directory &&
        !request.agent_config_directory->empty()) {
        load_user_prompts_adapter(*request.agent_config_directory, result, diagnostics, names);
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
            load_skills_adapter(fs, result, diagnostics, skill_names, SkillDirSpec{
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
            diagnostics,
            skill_names);
        if (request.agent_config_directory && !request.agent_config_directory->empty()) {
            load_user_skills_adapter(*request.agent_config_directory, result, diagnostics, skill_names);
        }
        load_user_agents_skills_adapter(user_agents_skills_dir, result, diagnostics, skill_names);
    }
    load_explicit_skill_paths(fs, request.skill_paths, result, diagnostics, skill_names);

    // 4. Project themes load only for a TUI assembly that opted in, while
    // the project is trusted.
    if (request.theme_resources_enabled && project_trusted) {
        load_project_themes_adapter(fs, result, diagnostics);
    }

    result.diagnostics = std::move(diagnostics);
    bound_diagnostics(result);
    return result;
}

} // namespace cch::coding_agent

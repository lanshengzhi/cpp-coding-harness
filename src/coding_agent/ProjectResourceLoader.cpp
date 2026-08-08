#include "coding_agent/ProjectResourceLoader.hpp"

#include "coding_agent/PromptTemplateLoader.hpp"
#include "coding_agent/ResourceDiagnosticPolicy.hpp"
#include "coding_agent/SkillLoader.hpp"
#include "../harness/WorkspaceFileSystem.hpp"

#include <algorithm>
#include <array>
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

void load_project_skills_adapter(
    const harness::WorkspaceFileSystem& fs,
    ProjectResourceLoadingResult& result,
    std::vector<ResourceDiagnostic>& diagnostics) {
    auto load = loadSkills(fs, {SkillDirSpec{.path = std::string{kProjectSkillsMarker}, .includeRootFiles = false}});
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
    result.resources.skills = std::move(load.skills);
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

    // 1. Detect `.pi/` markers. The mere presence of any loadable marker
    // triggers the Project Trust decision; the decision then gates all
    // project resource loading ("trusted means load").
    result.detection = detect_project_resources(fs);
    const bool trust_needed = needs_project_trust_resolution(result.detection);
    const auto workspace = request.workspace.empty() ? fs.root() : request.workspace;
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

    // 3. Project skills load only while the project is trusted
    // (`--no-skills` drops discovery; the explicit `--skill` path surface
    // lands with skill discovery).
    if (!request.no_skills && project_trusted) {
        load_project_skills_adapter(fs, result, diagnostics);
    }

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

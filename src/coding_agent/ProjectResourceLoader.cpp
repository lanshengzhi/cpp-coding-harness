#include "coding_agent/ProjectResourceLoader.hpp"

#include "coding_agent/PromptTemplateLoader.hpp"
#include "coding_agent/ResourceDiagnosticPolicy.hpp"
#include "coding_agent/SkillLoader.hpp"
#include "../harness/WorkspaceFileSystem.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <iterator>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace cch::coding_agent {
namespace {

struct LoadedResourceNames {
    std::set<std::string> skill_names;
    std::set<std::string> prompt_template_names;
};

struct ProjectResourceAdapter {
    ProjectResourceKind kind;
    const char* marker_path;
    bool (*plan_allows)(const ProjectResourceLoadPlan&);
    void (*load)(
        const harness::WorkspaceFileSystem&,
        ProjectResourceLoadingResult&,
        LoadedResourceNames&);
};

[[nodiscard]] ProjectResourceLoadingDiagnostic loading_diagnostic(
    ResourceDiagnosticSeverity severity,
    ProjectResourceLoadingDiagnosticCategory category,
    std::string code,
    std::string message,
    std::optional<std::string> path = std::nullopt,
    std::optional<ProjectResourceKind> kind = std::nullopt) {
    return ProjectResourceLoadingDiagnostic{
        .severity = severity,
        .category = category,
        .code = std::move(code),
        .message = std::move(message),
        .path = std::move(path),
        .kind = kind,
    };
}

[[nodiscard]] ResourceDiagnosticSeverity to_resource_severity(ProjectTrustDiagnosticSeverity severity) {
    switch (severity) {
    case ProjectTrustDiagnosticSeverity::Info:
        return ResourceDiagnosticSeverity::Info;
    case ProjectTrustDiagnosticSeverity::Warning:
        return ResourceDiagnosticSeverity::Warning;
    case ProjectTrustDiagnosticSeverity::Error:
        return ResourceDiagnosticSeverity::Error;
    }
    return ResourceDiagnosticSeverity::Warning;
}

[[nodiscard]] std::string to_code(SkillDiagnosticCode code) {
    switch (code) {
    case SkillDiagnosticCode::file_info_failed:
        return "file_info_failed";
    case SkillDiagnosticCode::list_failed:
        return "list_failed";
    case SkillDiagnosticCode::read_failed:
        return "read_failed";
    case SkillDiagnosticCode::parse_failed:
        return "parse_failed";
    case SkillDiagnosticCode::invalid_metadata:
        return "invalid_metadata";
    case SkillDiagnosticCode::duplicate_name:
        return "duplicate_name";
    }
    return "invalid_metadata";
}

[[nodiscard]] std::string to_code(PromptTemplateDiagnosticCode code) {
    switch (code) {
    case PromptTemplateDiagnosticCode::file_info_failed:
        return "file_info_failed";
    case PromptTemplateDiagnosticCode::list_failed:
        return "list_failed";
    case PromptTemplateDiagnosticCode::read_failed:
        return "read_failed";
    case PromptTemplateDiagnosticCode::parse_failed:
        return "parse_failed";
    case PromptTemplateDiagnosticCode::duplicate_name:
        return "duplicate_name";
    case PromptTemplateDiagnosticCode::unsupported_type:
        return "unsupported_type";
    }
    return "parse_failed";
}

void add_trust_diagnostics(
    ProjectResourceLoadingResult& result,
    const ProjectTrustResolution& trust) {
    for (const auto& diagnostic : trust.diagnostics) {
        result.diagnostics.push_back(loading_diagnostic(
            to_resource_severity(diagnostic.severity),
            ProjectResourceLoadingDiagnosticCategory::Trust,
            diagnostic.code,
            diagnostic.message,
            diagnostic.path));
    }
}

void add_load_plan_diagnostics(
    ProjectResourceLoadingResult& result,
    const ProjectResourceLoadPlan& plan) {
    for (const auto& diagnostic : plan.diagnostics) {
        result.diagnostics.push_back(loading_diagnostic(
            diagnostic.severity,
            ProjectResourceLoadingDiagnosticCategory::LoadPlan,
            diagnostic.code,
            diagnostic.message,
            diagnostic.path.empty() ? std::nullopt : std::optional<std::string>{diagnostic.path},
            diagnostic.kind));
    }

    for (const auto& decision : plan.decisions) {
        if (!decision.detected || decision.allowed || decision.reason == ResourceSkipReason::Unsupported) {
            continue;
        }
        result.diagnostics.push_back(loading_diagnostic(
            ResourceDiagnosticSeverity::Info,
            ProjectResourceLoadingDiagnosticCategory::LoadPlan,
            to_string(decision.kind),
            to_string(decision.kind) + " skipped: " + to_string(decision.reason),
            decision.path.empty() ? std::nullopt : std::optional<std::string>{decision.path},
            decision.kind));
    }
}

void add_skill_diagnostics(
    ProjectResourceLoadingResult& result,
    const SkillLoadResult& load,
    ProjectResourceKind kind) {
    for (const auto& diagnostic : load.diagnostics) {
        auto category = ProjectResourceLoadingDiagnosticCategory::SkillAdapter;
        auto code = to_code(diagnostic.code);
        if (diagnostic.code == SkillDiagnosticCode::duplicate_name) {
            category = ProjectResourceLoadingDiagnosticCategory::Duplicate;
            code = "duplicate_skill_skipped";
        }
        result.diagnostics.push_back(loading_diagnostic(
            ResourceDiagnosticSeverity::Warning,
            category,
            std::move(code),
            diagnostic.message,
            diagnostic.path.empty() ? std::nullopt : std::optional<std::string>{diagnostic.path},
            kind));
    }
}

void add_prompt_template_diagnostics(
    ProjectResourceLoadingResult& result,
    const PromptTemplateLoadResult& load,
    std::optional<ProjectResourceKind> kind) {
    for (const auto& diagnostic : load.diagnostics) {
        auto category = ProjectResourceLoadingDiagnosticCategory::PromptTemplateAdapter;
        auto code = to_code(diagnostic.code);
        if (diagnostic.code == PromptTemplateDiagnosticCode::duplicate_name) {
            category = ProjectResourceLoadingDiagnosticCategory::Duplicate;
            code = "duplicate_template_skipped";
        }
        result.diagnostics.push_back(loading_diagnostic(
            ResourceDiagnosticSeverity::Warning,
            category,
            std::move(code),
            diagnostic.message,
            diagnostic.path.empty() ? std::nullopt : std::optional<std::string>{diagnostic.path},
            kind));
    }
}

void append_project_skills(
    ProjectResourceLoadingResult& result,
    SkillLoadResult load,
    LoadedResourceNames& names) {
    add_skill_diagnostics(result, load, ProjectResourceKind::ProjectSkills);

    for (auto& skill : load.skills) {
        if (names.skill_names.contains(skill.name)) {
            result.diagnostics.push_back(loading_diagnostic(
                ResourceDiagnosticSeverity::Info,
                ProjectResourceLoadingDiagnosticCategory::Duplicate,
                "duplicate_skill_skipped",
                std::format(
                    "project skill '{}' skipped: earlier resource takes precedence",
                    skill.name),
                skill.filePath.empty() ? std::nullopt : std::optional<std::string>{skill.filePath},
                ProjectResourceKind::ProjectSkills));
            continue;
        }
        names.skill_names.insert(skill.name);
        result.resources.skills.push_back(std::move(skill));
    }
}

void append_prompt_templates(
    ProjectResourceLoadingResult& result,
    PromptTemplateLoadResult load,
    LoadedResourceNames& names,
    std::optional<ProjectResourceKind> kind) {
    add_prompt_template_diagnostics(result, load, kind);

    for (auto& tmpl : load.templates) {
        if (names.prompt_template_names.contains(tmpl.name)) {
            result.diagnostics.push_back(loading_diagnostic(
                ResourceDiagnosticSeverity::Info,
                ProjectResourceLoadingDiagnosticCategory::Duplicate,
                "duplicate_template_skipped",
                std::format(
                    "prompt template '{}' skipped: earlier resource takes precedence",
                    tmpl.name),
                std::nullopt,
                kind));
            continue;
        }
        names.prompt_template_names.insert(tmpl.name);
        result.resources.prompt_templates.push_back(std::move(tmpl));
    }
}

void load_project_skills_adapter(
    const harness::WorkspaceFileSystem& fs,
    ProjectResourceLoadingResult& result,
    LoadedResourceNames& names) {
    append_project_skills(
        result,
        loadSkills(fs, {SkillDirSpec{.path = ".cpp-harness/skills", .includeRootFiles = false}}),
        names);
}

void load_project_prompts_adapter(
    const harness::WorkspaceFileSystem& fs,
    ProjectResourceLoadingResult& result,
    LoadedResourceNames& names) {
    append_prompt_templates(
        result,
        loadPromptTemplates(
            fs,
            {PromptTemplateDirSpec{.path = ".cpp-harness/prompts", .is_file = false}}),
        names,
        ProjectResourceKind::ProjectPrompts);
}

void load_project_themes_adapter(
    const harness::WorkspaceFileSystem& fs,
    ProjectResourceLoadingResult& result,
    LoadedResourceNames& names) {
    (void)names;
    if (auto listed = fs.listDir(".cpp-harness/themes"); !listed) {
        result.diagnostics.push_back(loading_diagnostic(
            ResourceDiagnosticSeverity::Warning,
            ProjectResourceLoadingDiagnosticCategory::ThemeAdapter,
            "list_failed",
            listed.error().message,
            ".cpp-harness/themes",
            ProjectResourceKind::ProjectThemes));
    } else {
        std::sort(listed->begin(), listed->end(), [](const auto& left, const auto& right) {
            return left.name < right.name;
        });
        for (const auto& entry : *listed) {
            if (entry.kind != harness::FileKind::File || !entry.name.ends_with(".json")) continue;
            const auto path = ".cpp-harness/themes/" + entry.name;
            if (auto content = fs.readTextFile(path); !content) {
                result.diagnostics.push_back(loading_diagnostic(
                    ResourceDiagnosticSeverity::Warning,
                    ProjectResourceLoadingDiagnosticCategory::ThemeAdapter,
                    "read_failed",
                    content.error().message,
                    path,
                    ProjectResourceKind::ProjectThemes));
            } else {
                result.resources.project_themes.push_back({
                    .path = path,
                    .json = std::move(*content),
                });
            }
        }
    }
}

constexpr std::array<ProjectResourceAdapter, 3> kProjectResourceAdapters{{
    {
        .kind = ProjectResourceKind::ProjectSkills,
        .marker_path = ".cpp-harness/skills",
        .plan_allows = project_skills_allowed,
        .load = load_project_skills_adapter,
    },
    {
        .kind = ProjectResourceKind::ProjectPrompts,
        .marker_path = ".cpp-harness/prompts",
        .plan_allows = project_prompts_allowed,
        .load = load_project_prompts_adapter,
    },
    {
        .kind = ProjectResourceKind::ProjectThemes,
        .marker_path = ".cpp-harness/themes",
        .plan_allows = project_themes_allowed,
        .load = load_project_themes_adapter,
    },
}};

[[nodiscard]] ProjectResourcePolicy effective_policy_for(
    const ProjectResourceLoadingRequest& request) {
    auto policy = request.policy;
    if (!request.prompt_templates_enabled) {
        policy.project_prompts = ResourceEnablement::Off;
    }
    if (!request.theme_resources_enabled) {
        policy.project_themes = ResourceEnablement::Off;
    }
    return policy;
}

void bound_diagnostics(ProjectResourceLoadingResult& result) {
    const auto bound = [](auto& diagnostics) {
        for (auto& diagnostic : diagnostics) {
            detail::bound_resource_diagnostic_text(diagnostic.message);
            if (diagnostic.path) detail::bound_resource_diagnostic_text(*diagnostic.path);
        }
    };
    bound(result.diagnostics);
    bound(result.fatal_errors);

    if (result.diagnostics.size() <= detail::kMaxResourceDiagnostics) {
        return;
    }

    result.diagnostics.resize(detail::kMaxResourceDiagnostics - 1);
    result.diagnostics.push_back(loading_diagnostic(
        ResourceDiagnosticSeverity::Warning,
        ProjectResourceLoadingDiagnosticCategory::LoadPlan,
        "diagnostics_truncated",
        "Additional project resource diagnostics were omitted"));
}

} // namespace

ProjectResourceLoadingResult load_project_resources(
    const harness::WorkspaceFileSystem& fs,
    const ProjectTrustStore& trust_store,
    ProjectResourceLoadingRequest request) {
    ProjectResourceLoadingResult result;
    result.resources.skills = std::move(request.host_skills);
    result.resources.prompt_templates = std::move(request.host_prompt_templates);

    LoadedResourceNames names;
    for (const auto& skill : result.resources.skills) {
        names.skill_names.insert(skill.name);
    }
    for (const auto& tmpl : result.resources.prompt_templates) {
        names.prompt_template_names.insert(tmpl.name);
    }

    const auto policy = effective_policy_for(request);
    result.detection = detect_project_resources(fs);
    const bool trust_needed = needs_project_trust_resolution(result.detection, policy);
    const auto workspace = request.workspace.empty() ? fs.root() : request.workspace;
    result.trust = resolve_project_trust(
        workspace,
        trust_needed,
        trust_store,
        request.default_project_trust,
        request.project_trust_override);
    result.load_plan = build_project_resource_load_plan(
        result.detection,
        policy,
        result.trust);

    add_trust_diagnostics(result, result.trust);
    add_load_plan_diagnostics(result, result.load_plan);

    if (request.prompt_templates_enabled && !request.explicit_prompt_templates.empty()) {
        std::vector<PromptTemplate> explicit_templates;
        for (auto& input : request.explicit_prompt_templates) {
            auto input_path = std::move(input.path);
            auto explicit_load = loadPromptTemplates(
                fs,
                {PromptTemplateDirSpec{
                    .path = input_path,
                    .is_file = input.is_file,
                }});

            if (explicit_load.templates.empty() && explicit_load.diagnostics.empty()) {
                result.fatal_errors.push_back(loading_diagnostic(
                    ResourceDiagnosticSeverity::Error,
                    ProjectResourceLoadingDiagnosticCategory::PromptTemplateAdapter,
                    "no_templates_found",
                    "explicit prompt template input contains no loadable .md files",
                    input_path));
            }

            for (auto& diag : explicit_load.diagnostics) {
                if (diag.code == PromptTemplateDiagnosticCode::duplicate_name) {
                    result.diagnostics.push_back(loading_diagnostic(
                        ResourceDiagnosticSeverity::Warning,
                        ProjectResourceLoadingDiagnosticCategory::Duplicate,
                        "duplicate_template_skipped",
                        diag.message,
                        diag.path.empty() ? std::nullopt : std::optional<std::string>{diag.path}));
                } else {
                    result.fatal_errors.push_back(loading_diagnostic(
                        ResourceDiagnosticSeverity::Error,
                        ProjectResourceLoadingDiagnosticCategory::PromptTemplateAdapter,
                        to_code(diag.code),
                        diag.message,
                        diag.path.empty() ? std::nullopt : std::optional<std::string>{diag.path}));
                }
            }

            for (auto& tmpl : explicit_load.templates) {
                if (names.prompt_template_names.contains(tmpl.name)) {
                    result.diagnostics.push_back(loading_diagnostic(
                        ResourceDiagnosticSeverity::Info,
                        ProjectResourceLoadingDiagnosticCategory::Duplicate,
                        "duplicate_template_skipped",
                        std::format(
                            "explicit prompt template '{}' skipped: earlier resource takes precedence",
                            tmpl.name),
                        input_path));
                    continue;
                }
                names.prompt_template_names.insert(tmpl.name);
                explicit_templates.push_back(std::move(tmpl));
            }
        }

        std::sort(
            explicit_templates.begin(),
            explicit_templates.end(),
            [](const PromptTemplate& left, const PromptTemplate& right) {
                return left.name < right.name;
            });
        result.resources.prompt_templates.insert(
            result.resources.prompt_templates.end(),
            std::make_move_iterator(explicit_templates.begin()),
            std::make_move_iterator(explicit_templates.end()));
    }

    for (const auto& adapter : kProjectResourceAdapters) {
        if (adapter.plan_allows(result.load_plan)) {
            adapter.load(fs, result, names);
        }
    }

    bound_diagnostics(result);
    return result;
}

std::string_view project_resource_loading_diagnostic_category_name(
    ProjectResourceLoadingDiagnosticCategory category) {
    switch (category) {
    case ProjectResourceLoadingDiagnosticCategory::Trust:
        return "trust";
    case ProjectResourceLoadingDiagnosticCategory::LoadPlan:
        return "resource";
    case ProjectResourceLoadingDiagnosticCategory::SkillAdapter:
        return "skill";
    case ProjectResourceLoadingDiagnosticCategory::PromptTemplateAdapter:
        return "template";
    case ProjectResourceLoadingDiagnosticCategory::ThemeAdapter:
        return "theme";
    case ProjectResourceLoadingDiagnosticCategory::Duplicate:
        return "duplicate";
    }
    return "resource";
}

std::string project_resource_loading_diagnostic_code(
    const ProjectResourceLoadingDiagnostic& diagnostic) {
    return std::string{project_resource_loading_diagnostic_category_name(diagnostic.category)} +
           ":" + diagnostic.code;
}

} // namespace cch::coding_agent

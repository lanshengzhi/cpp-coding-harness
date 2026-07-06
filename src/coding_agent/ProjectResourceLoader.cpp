#include "coding_agent/ProjectResourceLoader.hpp"

#include "coding_agent/PromptTemplateLoader.hpp"
#include "coding_agent/SkillLoader.hpp"
#include "../harness/WorkspaceFileSystem.hpp"

#include <array>
#include <format>
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
    }
    return "parse_failed";
}

void add_trust_diagnostics(
    ProjectResourceLoadingResult& result,
    const ProjectTrustResolution& trust) {
    for (const auto& diagnostic : trust.diagnostics) {
        result.diagnostics.push_back(ProjectResourceLoadingDiagnostic{
            .severity = to_resource_severity(diagnostic.severity),
            .source = ProjectResourceLoadingDiagnosticSource::Trust,
            .code = diagnostic.code,
            .message = diagnostic.message,
            .path = diagnostic.path,
            .kind = std::nullopt,
        });
    }
}

void add_load_plan_diagnostics(
    ProjectResourceLoadingResult& result,
    const ProjectResourceLoadPlan& plan) {
    for (const auto& diagnostic : plan.diagnostics) {
        result.diagnostics.push_back(ProjectResourceLoadingDiagnostic{
            .severity = diagnostic.severity,
            .source = ProjectResourceLoadingDiagnosticSource::LoadPlan,
            .code = diagnostic.code,
            .message = diagnostic.message,
            .path = diagnostic.path.empty() ? std::nullopt : std::optional<std::string>{diagnostic.path},
            .kind = diagnostic.kind,
        });
    }

    for (const auto& decision : plan.decisions) {
        if (!decision.detected || decision.allowed || decision.reason == ResourceSkipReason::Unsupported) {
            continue;
        }
        result.diagnostics.push_back(ProjectResourceLoadingDiagnostic{
            .severity = ResourceDiagnosticSeverity::Info,
            .source = ProjectResourceLoadingDiagnosticSource::LoadPlan,
            .code = to_string(decision.kind),
            .message = to_string(decision.kind) + " skipped: " + to_string(decision.reason),
            .path = decision.path.empty() ? std::nullopt : std::optional<std::string>{decision.path},
            .kind = decision.kind,
        });
    }
}

void add_skill_diagnostics(
    ProjectResourceLoadingResult& result,
    const SkillLoadResult& load,
    ProjectResourceKind kind) {
    for (const auto& diagnostic : load.diagnostics) {
        result.diagnostics.push_back(ProjectResourceLoadingDiagnostic{
            .severity = ResourceDiagnosticSeverity::Warning,
            .source = ProjectResourceLoadingDiagnosticSource::SkillAdapter,
            .code = to_code(diagnostic.code),
            .message = diagnostic.message,
            .path = diagnostic.path.empty() ? std::nullopt : std::optional<std::string>{diagnostic.path},
            .kind = kind,
        });
    }
}

void add_prompt_template_diagnostics(
    ProjectResourceLoadingResult& result,
    const PromptTemplateLoadResult& load,
    std::optional<ProjectResourceKind> kind) {
    for (const auto& diagnostic : load.diagnostics) {
        result.diagnostics.push_back(ProjectResourceLoadingDiagnostic{
            .severity = ResourceDiagnosticSeverity::Warning,
            .source = ProjectResourceLoadingDiagnosticSource::PromptTemplateAdapter,
            .code = to_code(diagnostic.code),
            .message = diagnostic.message,
            .path = diagnostic.path.empty() ? std::nullopt : std::optional<std::string>{diagnostic.path},
            .kind = kind,
        });
    }
}

void append_project_skills(
    ProjectResourceLoadingResult& result,
    SkillLoadResult load,
    LoadedResourceNames& names) {
    add_skill_diagnostics(result, load, ProjectResourceKind::ProjectSkills);

    for (auto& skill : load.skills) {
        if (names.skill_names.contains(skill.name)) {
            result.diagnostics.push_back(ProjectResourceLoadingDiagnostic{
                .severity = ResourceDiagnosticSeverity::Info,
                .source = ProjectResourceLoadingDiagnosticSource::Duplicate,
                .code = "duplicate_skill_skipped",
                .message = std::format(
                    "project skill '{}' skipped: earlier resource takes precedence",
                    skill.name),
                .path = skill.filePath.empty() ? std::nullopt : std::optional<std::string>{skill.filePath},
                .kind = ProjectResourceKind::ProjectSkills,
            });
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
            result.diagnostics.push_back(ProjectResourceLoadingDiagnostic{
                .severity = ResourceDiagnosticSeverity::Info,
                .source = ProjectResourceLoadingDiagnosticSource::Duplicate,
                .code = "duplicate_template_skipped",
                .message = std::format(
                    "prompt template '{}' skipped: earlier resource takes precedence",
                    tmpl.name),
                .path = std::nullopt,
                .kind = kind,
            });
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

constexpr std::array<ProjectResourceAdapter, 2> kProjectResourceAdapters{{
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
}};

[[nodiscard]] ProjectResourcePolicy effective_policy_for(
    const ProjectResourceLoadingRequest& request) {
    auto policy = request.policy;
    if (!request.prompt_templates_enabled) {
        policy.project_prompts = ResourceEnablement::Off;
    }
    return policy;
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

    for (const auto& adapter : kProjectResourceAdapters) {
        if (adapter.plan_allows(result.load_plan)) {
            adapter.load(fs, result, names);
        }
    }

    if (request.prompt_templates_enabled && !request.explicit_prompt_templates.empty()) {
        std::vector<PromptTemplateDirSpec> specs;
        specs.reserve(request.explicit_prompt_templates.size());
        for (auto& input : request.explicit_prompt_templates) {
            specs.push_back(PromptTemplateDirSpec{
                .path = std::move(input.path),
                .is_file = input.is_file,
            });
        }
        append_prompt_templates(
            result,
            loadPromptTemplates(fs, specs),
            names,
            std::nullopt);
    }

    return result;
}

} // namespace cch::coding_agent

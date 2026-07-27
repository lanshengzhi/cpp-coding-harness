#pragma once

#include <cch/coding_agent/ProjectResources.hpp>
#include <cch/coding_agent/ProjectTrust.hpp>
#include <cch/coding_agent/PromptTemplate.hpp>
#include <cch/coding_agent/Skill.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cch::harness {
class WorkspaceFileSystem;
}

namespace cch::coding_agent {

enum class ProjectResourceLoadingDiagnosticCategory {
    Trust,
    LoadPlan,
    SkillAdapter,
    PromptTemplateAdapter,
    ThemeAdapter,
    Duplicate,
};

struct ProjectResourceLoadingDiagnostic {
    ResourceDiagnosticSeverity severity{ResourceDiagnosticSeverity::Warning};
    ProjectResourceLoadingDiagnosticCategory category{ProjectResourceLoadingDiagnosticCategory::LoadPlan};
    std::string code;
    std::string message;
    std::optional<std::string> path;
    std::optional<ProjectResourceKind> kind;
};

struct ExplicitPromptTemplateInput {
    std::string path;
    bool is_file{true};
};

struct LoadedProjectThemeResource {
    std::string path;
    std::string json;
};

struct LoadedProjectResources {
    std::vector<Skill> skills;
    std::vector<PromptTemplate> prompt_templates;
    /// Trusted, contained project theme documents. Parsing stays in the
    /// physically separate coding-agent TUI package.
    std::vector<LoadedProjectThemeResource> project_themes;
};

struct ProjectResourceLoadingRequest {
    std::filesystem::path workspace;
    ProjectResourcePolicy policy;
    DefaultProjectTrust default_project_trust{DefaultProjectTrust::Ask};
    std::optional<bool> project_trust_override;
    bool prompt_templates_enabled{true};
    /// False for every existing non-TUI assembly. A Native TUI assembly opts
    /// in only after it has decided to consume theme resources.
    bool theme_resources_enabled{false};
    std::vector<Skill> host_skills;
    std::vector<PromptTemplate> host_prompt_templates;
    std::vector<ExplicitPromptTemplateInput> explicit_prompt_templates;
};

struct ProjectResourceLoadingResult {
    ProjectResourceDetectionResult detection;
    ProjectTrustResolution trust;
    ProjectResourceLoadPlan load_plan;
    LoadedProjectResources resources;
    /// Errors that must abort session creation (e.g. explicit resource failures).
    std::vector<ProjectResourceLoadingDiagnostic> fatal_errors;
    std::vector<ProjectResourceLoadingDiagnostic> diagnostics;
};

[[nodiscard]] ProjectResourceLoadingResult load_project_resources(
    const harness::WorkspaceFileSystem& fs,
    const ProjectTrustStore& trust_store,
    ProjectResourceLoadingRequest request);

[[nodiscard]] std::string_view project_resource_loading_diagnostic_category_name(
    ProjectResourceLoadingDiagnosticCategory category);

[[nodiscard]] std::string project_resource_loading_diagnostic_code(
    const ProjectResourceLoadingDiagnostic& diagnostic);

} // namespace cch::coding_agent

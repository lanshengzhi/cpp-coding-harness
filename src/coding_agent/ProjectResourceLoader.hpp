#pragma once

#include <cch/coding_agent/ProjectResources.hpp>
#include <cch/coding_agent/ProjectTrust.hpp>
#include <cch/coding_agent/PromptTemplate.hpp>
#include <cch/coding_agent/Skill.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace cch::harness {
class WorkspaceFileSystem;
}

namespace cch::coding_agent {

enum class ProjectResourceLoadingDiagnosticSource {
    Trust,
    LoadPlan,
    SkillAdapter,
    PromptTemplateAdapter,
    Duplicate,
};

struct ProjectResourceLoadingDiagnostic {
    ResourceDiagnosticSeverity severity{ResourceDiagnosticSeverity::Warning};
    ProjectResourceLoadingDiagnosticSource source{ProjectResourceLoadingDiagnosticSource::LoadPlan};
    std::string code;
    std::string message;
    std::optional<std::string> path;
    std::optional<ProjectResourceKind> kind;
};

struct ExplicitPromptTemplateInput {
    std::string path;
    bool is_file{true};
};

struct LoadedProjectResources {
    std::vector<Skill> skills;
    std::vector<PromptTemplate> prompt_templates;
};

struct ProjectResourceLoadingRequest {
    std::filesystem::path workspace;
    ProjectResourcePolicy policy;
    DefaultProjectTrust default_project_trust{DefaultProjectTrust::Ask};
    std::optional<bool> project_trust_override;
    bool prompt_templates_enabled{true};
    std::vector<Skill> host_skills;
    std::vector<PromptTemplate> host_prompt_templates;
    std::vector<ExplicitPromptTemplateInput> explicit_prompt_templates;
};

struct ProjectResourceLoadingResult {
    ProjectResourceDetectionResult detection;
    ProjectTrustResolution trust;
    ProjectResourceLoadPlan load_plan;
    LoadedProjectResources resources;
    std::vector<ProjectResourceLoadingDiagnostic> diagnostics;
};

[[nodiscard]] ProjectResourceLoadingResult load_project_resources(
    const harness::WorkspaceFileSystem& fs,
    const ProjectTrustStore& trust_store,
    ProjectResourceLoadingRequest request);

} // namespace cch::coding_agent

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

/// Project resource loading request — the pi `DefaultResourceLoader` subset
/// without extensions/package-manager (ADR 0036 G4): `.pi/` markers, no
/// per-resource enablement policy ("trusted means load", #327),
/// trust-deferred loading at session creation and on `/reload`.
struct ProjectResourceLoadingRequest {
    std::filesystem::path workspace;
    /// User-level resource root (`~/.pi/agent`). User prompt templates load
    /// from `<agent_config_directory>/prompts`; an empty path skips them.
    std::optional<std::filesystem::path> agent_config_directory;
    DefaultProjectTrust default_project_trust{DefaultProjectTrust::Ask};
    std::optional<bool> project_trust_override;
    /// pi `--no-skills`: drops user and project skill discovery (explicit
    /// `--skill` paths stay; the explicit-path surface lands with skill
    /// discovery).
    bool no_skills{false};
    /// pi `--no-prompt-templates`: drops user and project prompt discovery
    /// (explicit `--prompt-template` paths still load).
    bool no_prompt_templates{false};
    /// False for every existing non-TUI assembly. A Native TUI assembly opts
    /// in only after it has decided to consume theme resources.
    bool theme_resources_enabled{false};
    std::vector<ExplicitPromptTemplateInput> explicit_prompt_templates;
};

struct ProjectResourceLoadingResult {
    ProjectResourceDetectionResult detection;
    ProjectTrustResolution trust;
    LoadedProjectResources resources;
    /// Errors that must abort session creation (e.g. explicit resource failures).
    std::vector<ResourceDiagnostic> fatal_errors;
    std::vector<ResourceDiagnostic> diagnostics;
};

[[nodiscard]] ProjectResourceLoadingResult load_project_resources(
    const harness::WorkspaceFileSystem& fs,
    const ProjectTrustStore& trust_store,
    ProjectResourceLoadingRequest request);

} // namespace cch::coding_agent

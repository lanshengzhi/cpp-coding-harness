#pragma once

#include <cch/coding_agent/ProjectResources.hpp>
#include <cch/coding_agent/ProjectTrust.hpp>
#include <cch/coding_agent/PromptTemplate.hpp>
#include <cch/coding_agent/Skill.hpp>
#include "coding_agent/prompt/SystemPromptBuilder.hpp"

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

struct LoadedProjectResources {
    std::vector<Skill> skills;
    std::vector<PromptTemplate> prompt_templates;
    /// Theme documents in pi's load order: trust-gated project `.pi/themes`,
    /// user `<agent_config_directory>/themes`, then explicit `--theme`
    /// paths (discovered before explicit, pi's merge order). The document
    /// contract lives in the public `ProjectResources.hpp` header so the
    /// coding-agent TUI package can consume it without a runtime dependency.
    std::vector<LoadedThemeResource> themes;
    /// pi `getAgentsFiles()`: Project Context Files in pi's load order — the
    /// global context file from the Agent Config Directory, then the cwd
    /// ancestor chain root-most first (linked-worktree shadowing applied).
    /// Not Project Trust gated (pinned fact) and disabled by
    /// `--no-context-files`.
    std::vector<prompt::ProjectContextFile> agents_files;
    /// pi `getSystemPrompt()`: the resolved custom system prompt text — the
    /// CLI `--system-prompt` value (text-or-file per `resolvePromptInput`),
    /// else the discovered SYSTEM.md content (project `.pi/` trust-gated,
    /// then the global Agent Config Directory).
    std::optional<std::string> system_prompt;
    /// pi `getAppendSystemPrompt()`: the resolved append strings in source
    /// order (CLI `--append-system-prompt` values, else the discovered
    /// APPEND_SYSTEM.md content). The System Prompt joins them with
    /// `"\n\n"`.
    std::vector<std::string> append_system_prompt;
    /// pi `getSystemPromptSource()`: the SYSTEM.md source path when the
    /// resolved custom prompt came from a file (loaded-resources Context
    /// presentation).
    std::optional<std::string> system_prompt_source;
    /// pi `getAppendSystemPromptSources()`: the APPEND_SYSTEM.md source
    /// paths when the resolved append strings came from files.
    std::vector<std::string> append_system_prompt_sources;
};

/// Project resource loading request — the pi `DefaultResourceLoader` subset
/// without extensions/package-manager (ADR 0036 G4): `.pi/` markers, no
/// per-resource enablement policy ("trusted means load", #327),
/// trust-deferred loading at session creation and on `/reload`.
struct ProjectResourceLoadingRequest {
    std::filesystem::path workspace;
    /// User-level resource root (`~/.pi/agent`). User skills load from
    /// `<agent_config_directory>/skills` (pi discovery mode, root-level `.md`
    /// included) and user prompt templates from
    /// `<agent_config_directory>/prompts`; an empty path skips them.
    std::optional<std::filesystem::path> agent_config_directory;
    /// The user's home directory, used for the `~/.agents/skills` convention
    /// (user skills load; the directory is excluded from the project
    /// `.agents/skills` ancestor walk). Defaults to `home_directory()`.
    std::optional<std::filesystem::path> home_directory;
    DefaultProjectTrust default_project_trust{DefaultProjectTrust::Ask};
    std::optional<bool> project_trust_override;
    /// pi `--no-skills`: drops user and project skill discovery (explicit
    /// `--skill` paths stay).
    bool no_skills{false};
    /// pi `--no-prompt-templates`: drops user and project prompt discovery
    /// (explicit `--prompt-template` paths still load).
    bool no_prompt_templates{false};
    /// pi `--no-themes`: drops user `<agent_config_directory>/themes` and
    /// trust-gated project `.pi/themes` discovery (explicit `--theme`
    /// paths still load).
    bool no_themes{false};
    /// pi `--no-context-files`: disables Project Context File discovery
    /// (global AGENTS.md/CLAUDE.md + the cwd ancestor chain). Context files
    /// are never Project Trust gated (pinned fact).
    bool no_context_files{false};
    /// pi `--system-prompt`: the raw text-or-file value. Wins over SYSTEM.md
    /// discovery even when empty (pi `systemPromptSource ?? discovery`); an
    /// empty value resolves to no custom prompt.
    std::optional<std::string> system_prompt;
    /// pi `--append-system-prompt` (repeatable): raw text-or-file values.
    /// Win over APPEND_SYSTEM.md discovery when non-empty (pi
    /// `appendSystemPromptSource ?? discovery`); each entry resolves through
    /// pi's `resolvePromptInput`.
    std::vector<std::string> append_system_prompt;
    /// Repeatable pi `--theme` paths (files or directories): explicit theme
    /// inputs load after every discovered source (discovered themes win
    /// name collisions, pi's merge order) and stay effective under
    /// `--no-themes`. Missing inputs carry pi's two diagnostics (the loader
    /// warning and the resource-loader error, both non-fatal).
    std::vector<std::string> theme_paths;
    /// Repeatable pi `--skill` paths (files or directories): explicit skills
    /// load after every discovered source (discovered skills win name
    /// collisions, pi's merge order) and stay effective under `--no-skills`.
    std::vector<std::string> skill_paths;
    /// Repeatable pi `--prompt-template` inputs.
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

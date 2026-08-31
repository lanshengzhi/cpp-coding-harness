#pragma once

#include <cch/agent/AgentContext.hpp>
#include <cch/ai/Model.hpp>
#include <cch/coding_agent/ModelResolver.hpp>
#include <cch/coding_agent/PromptTemplate.hpp>
#include <cch/coding_agent/Skill.hpp>
#include "coding_agent/ProjectResourceLoader.hpp"
#include "coding_agent/prompt/SystemPromptBuilder.hpp"
#include "coding_agent/runtime/RuntimeServices.hpp"
#include "coding_agent/runtime/SessionLifecycle.hpp"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace cch::coding_agent::runtime {

/// Agent Session configuration resolved by Session Assembly (pi
/// `AgentSessionConfig` subset): queue bounds, the resolved Model, and the
/// resource-loader inputs the System Prompt is built from.
struct AgentSessionConfig {
    std::size_t max_queued_messages{agent::kDefaultMaxQueuedMessages};
    std::size_t max_queued_bytes{agent::kDefaultMaxQueuedBytes};
    /// Explicit turn cap forwarded to the Agent; std::nullopt imposes no cap
    /// (ADR 0015).
    std::optional<int> max_turns{std::nullopt};
    ai::Model model{};
    /// pi `scopedModels`: the `--models` scope carried into the session for
    /// Ctrl+P cycling. Session-only state; the interactive scoped-models
    /// selector replaces it at runtime.
    std::vector<coding_agent::ScopedModel> scoped_models{};
    /// pi `defaultThinkingLevel` from the merged settings scope. Used as the
    /// fresh-session and resumed-without-entry thinking level before the
    /// Agent's creation clamp (pi sdk.ts
    /// `settingsManager.getDefaultThinkingLevel() ?? DEFAULT_THINKING_LEVEL`);
    /// a resumed `thinking_level_change` entry wins over it (T04).
    std::optional<std::string> default_thinking_level{std::nullopt};
    /// pi `_rebuildSystemPrompt` inputs from the resource loader
    /// (`resourceLoader.getSystemPrompt()`): the custom system prompt text
    /// (`--system-prompt` text-or-file, else the discovered SYSTEM.md
    /// content) rendering as the custom-prompt branch.
    std::optional<std::string> custom_prompt{std::nullopt};
    /// pi `resourceLoader.getAppendSystemPrompt()`: the resolved append
    /// strings in source order; joined with `"\n\n"` into the append
    /// section (pi `_rebuildSystemPrompt`).
    std::vector<std::string> append_system_prompt;
    /// pi `resourceLoader.getAgentsFiles().agentsFiles`: the Project Context
    /// Files rendering as `<project_context>`/`<project_instructions
    /// path="...">`. Not Project Trust gated (pinned fact).
    std::vector<prompt::ProjectContextFile> context_files;
    /// pi `resourceLoader.getSystemPromptSource()`: the SYSTEM.md source
    /// path when the resolved custom prompt came from a file (loaded-resources
    /// Context presentation).
    std::optional<std::string> system_prompt_source{std::nullopt};
    /// pi `resourceLoader.getAppendSystemPromptSources()`: the
    /// APPEND_SYSTEM.md source paths when the append strings came from files.
    std::vector<std::string> append_system_prompt_sources;
    /// Per-kind loader diagnostics (pi `skillDiagnostics`/
    /// `promptDiagnostics`/`themeDiagnostics`) for the loaded-resources
    /// presentation and the `/reload` refresh.
    std::vector<ResourceDiagnostic> skill_diagnostics;
    std::vector<ResourceDiagnostic> prompt_diagnostics;
    std::vector<ResourceDiagnostic> theme_diagnostics;
    /// The resolved `ProjectResourceLoadingRequest` the session was assembled
    /// under (pi's retained `DefaultResourceLoader` options): `/reload`
    /// re-runs the same discovery with the creation-time trust state
    /// preserved.
    std::optional<ProjectResourceLoadingRequest> resource_loading_request{std::nullopt};
    /// The composition-authorized filesystem capabilities used for the
    /// creation-time load. Retained so `/reload` reuses the same roots rather
    /// than manufacturing a new or broader filesystem authority.
    ProjectResourceFileSystems resource_file_systems{};
};

/// The one private move-only passive assembly value handed from Session
/// Assembly (SessionFactory, the sole Session Assembly module) to
/// AgentSession, which constructs its own Impl from it. Carries the Runtime
/// Services, the opened Agent Session, the loaded Skills and Prompt
/// Templates, the Agent Session configuration, and the resolved session path.
/// `session_path` is recorded at assembly so introspection (snapshot(),
/// session_path()) survives Session Close's Session Store release.
struct AgentSessionAssembly {
    RuntimeServices services;
    OpenSession session;
    std::vector<Skill> skills;
    std::vector<PromptTemplate> templates;
    AgentSessionConfig config;
    std::optional<std::filesystem::path> session_path;
};

} // namespace cch::coding_agent::runtime

#pragma once

// Prompt processing is now split into focused modules. This header remains a
// compatibility umbrella; new code should include the specific module headers.

#include "CommandRegistry.hpp"
#include "PromptProcessingPipeline.hpp"
#include "PromptTemplate.hpp"
#include "PromptTemplateExpander.hpp"
#include "SkillExpander.hpp"

#include "Skill.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace cch::coding_agent {

// ── Backward-compatible free functions ──
// These delegate to the PromptProcessingPipeline / adapters below.

/// Expand a skill command (/skill:name args) to its full <skill> XML block.
/// Returns the expanded text, or the original input if no skill matched.
/// Prints diagnostics to stderr for unknown skills and file read failures.
[[nodiscard]] std::string expand_skill_command(
    std::string_view input,
    const std::vector<Skill>& skills,
    const harness::WorkspaceFileSystem& fs);

/// Expand a skill command without writing to stderr.
/// Diagnostics are returned as values for SDK/non-interactive use.
[[nodiscard]] SkillExpansionResult expand_skill_command_silent(
    std::string_view input,
    const std::vector<Skill>& skills);

/// Expand a prompt template if input matches `/templateName args`.
/// Returns the expanded text, or the original input if no match.
[[nodiscard]] std::string expand_prompt_template(
    std::string_view input,
    const std::vector<PromptTemplate>& templates);

/// Process raw user input: dispatch slash-commands, expand prompt templates.
/// Called before the agent loop in both REPL and RPC paths.
[[nodiscard]] PromptProcessingResult process_prompt(
    std::string_view raw_input,
    const std::vector<PromptTemplate>& templates,
    CommandRegistry& registry,
    const CommandContext& ctx = {});

/// Process raw user input with skill expansion support.
/// Expands /skill:name inline before slash-command dispatch.
[[nodiscard]] PromptProcessingResult process_prompt(
    std::string_view raw_input,
    const std::vector<PromptTemplate>& templates,
    CommandRegistry& registry,
    const CommandContext& ctx,
    const std::vector<Skill>& skills,
    const harness::WorkspaceFileSystem& fs);

} // namespace cch::coding_agent

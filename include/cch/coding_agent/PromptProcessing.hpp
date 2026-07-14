#pragma once

#include "CommandRegistry.hpp"
#include "PromptTemplate.hpp"
#include "Skill.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cch::harness {
class WorkspaceFileSystem;
}

namespace cch::coding_agent {

/// Legacy compatibility result for prompt processing.
/// New production code uses the private two-alternative PromptProcessingOutcome.
struct PromptProcessingResult {
    bool command_handled{false};
    std::optional<std::string> display_text;
    std::string expanded_prompt;
    bool shutdown_requested{false};
};

/// Legacy compatibility result for silent skill expansion.
struct SkillExpansionResult {
    std::string expanded;
    std::vector<std::string> diagnostics;
};

/// Legacy compatibility wrapper for `/skill:name` expansion.
/// The filesystem argument is retained for source compatibility; expansion uses
/// the already-authorized cached Skill::content snapshot and never prints.
[[nodiscard]] std::string expand_skill_command(
    std::string_view input,
    const std::vector<Skill>& skills,
    const harness::WorkspaceFileSystem& fs);

/// Legacy compatibility wrapper returning diagnostics as values.
/// Unknown skills pass through silently, so diagnostics are currently empty.
[[nodiscard]] SkillExpansionResult expand_skill_command_silent(
    std::string_view input,
    const std::vector<Skill>& skills);

/// Expand a prompt template if input matches `/templateName args` at column zero.
[[nodiscard]] std::string expand_prompt_template(
    std::string_view input,
    const std::vector<PromptTemplate>& templates);

/// Legacy compatibility wrapper for command/template prompt processing.
[[nodiscard]] PromptProcessingResult process_prompt(
    std::string_view raw_input,
    const std::vector<PromptTemplate>& templates,
    CommandRegistry& registry,
    const CommandContext& ctx = {});

/// Legacy compatibility wrapper with skill expansion support.
/// The filesystem argument is retained for source compatibility only.
[[nodiscard]] PromptProcessingResult process_prompt(
    std::string_view raw_input,
    const std::vector<PromptTemplate>& templates,
    CommandRegistry& registry,
    const CommandContext& ctx,
    const std::vector<Skill>& skills,
    const harness::WorkspaceFileSystem& fs);

} // namespace cch::coding_agent

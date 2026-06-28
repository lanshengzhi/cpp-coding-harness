#include "../../include/cch/coding_agent/CommandRegistry.hpp"
#include "../../include/cch/coding_agent/PromptProcessingPipeline.hpp"
#include "../../include/cch/coding_agent/PromptTemplate.hpp"
#include "../../include/cch/coding_agent/PromptTemplateExpander.hpp"
#include "../../include/cch/coding_agent/SkillExpander.hpp"

#include <string_view>

namespace cch::coding_agent {
namespace {

[[nodiscard]] std::string_view trim_left(std::string_view sv) {
    while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t')) {
        sv.remove_prefix(1);
    }
    return sv;
}

[[nodiscard]] std::string_view extract_command_name(std::string_view input) {
    auto trimmed = trim_left(input);
    if (trimmed.empty() || trimmed.front() != '/') return {};
    trimmed.remove_prefix(1); // strip '/'
    auto end = trimmed.find_first_of(" \t\n\r");
    return trimmed.substr(0, end);
}

[[nodiscard]] std::string_view extract_args(std::string_view input) {
    auto trimmed = trim_left(input);
    if (trimmed.empty() || trimmed.front() != '/') return {};
    trimmed.remove_prefix(1);
    auto space = trimmed.find_first_of(" \t");
    if (space == std::string_view::npos) return {};
    return trim_left(trimmed.substr(space + 1));
}

} // namespace

PromptProcessingPipeline::PromptProcessingPipeline(
    const PromptTemplateExpander& template_expander,
    CommandRegistry& command_registry,
    const SkillExpander& skill_expander)
    : template_expander_(template_expander),
      command_registry_(command_registry),
      skill_expander_(skill_expander) {}

PromptProcessingResult PromptProcessingPipeline::process(
    std::string_view raw_input,
    const CommandContext& ctx) const {
    // Expand /skill:name inline before command dispatch
    auto expanded_skill = skill_expander_.expand_and_print(raw_input);
    if (expanded_skill != raw_input) {
        PromptProcessingResult result;
        result.command_handled = false;
        result.expanded_prompt = std::move(expanded_skill);
        return result;
    }

    PromptProcessingResult result;
    auto trimmed = trim_left(raw_input);

    // Detect ! shell passthrough
    if (!trimmed.empty() && trimmed.front() == '!') {
        result.command_handled = true;
        result.display_text = "Shell passthrough (!) is not yet implemented.";
        return result;
    }

    // Detect slash-command
    if (!trimmed.empty() && trimmed.front() == '/') {
        auto name = extract_command_name(trimmed);
        auto args = extract_args(trimmed);

        if (name.empty()) {
            result.command_handled = true;
            result.display_text = "Commands start with / followed by a command name. Try /session or /quit.";
            return result;
        }

        // Dispatch to command registry
        auto cmd_result = command_registry_.dispatch(name, ctx, args);
        if (cmd_result) {
            result.command_handled = true;
            result.display_text = std::move(cmd_result->display_text);
            result.shutdown_requested = cmd_result->shutdown_requested;
            return result;
        }

        // No command matched — try template expansion
        auto expanded = template_expander_.expand(trimmed);
        if (expanded != trimmed) {
            result.command_handled = false;
            result.expanded_prompt = std::move(expanded);
            return result;
        }

        // Unknown command
        result.command_handled = true;
        result.display_text = std::string{"Unknown command: /"} + std::string{name};
        return result;
    }

    // No command — pass through unchanged
    result.command_handled = false;
    result.expanded_prompt = std::string{raw_input};
    return result;
}

} // namespace cch::coding_agent

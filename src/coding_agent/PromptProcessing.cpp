#include "../../include/cch/coding_agent/PromptProcessing.hpp"

#include "../../include/cch/coding_agent/SkillFormatting.hpp"
#include "../coding_agent/SkillFrontmatterParser.hpp"
#include "../harness/WorkspaceFileSystem.hpp"

#include <iostream>

namespace cch::coding_agent {

void register_builtin_commands(CommandRegistry& registry) {
    // /session — print current session info
    registry.register_command("session", [](const CommandContext& ctx, std::string_view /*args*/) {
        std::string text;
        text += "Session: " + ctx.session_id + "\n";
        text += "Workspace: " + ctx.workspace_path + "\n";
        text += "Provider: " + ctx.provider + "\n";
        text += "Model: " + ctx.model + "\n";
        text += "Messages: " + std::to_string(ctx.message_count);
        return CommandResult{std::move(text)};
    });

    // /quit — signal shutdown
    registry.register_command("quit", [](const CommandContext& /*ctx*/, std::string_view /*args*/) {
        return CommandResult{"Shutting down.", true};
    });

    // /new — start a new session (returns instruction text)
    registry.register_command("new", [](const CommandContext& /*ctx*/, std::string_view /*args*/) {
        return CommandResult{"To start a new session, restart cpp-harness without --resume."};
    });

    // /resume <session-id> — resume a previous session
    registry.register_command("resume", [](const CommandContext& /*ctx*/, std::string_view args) {
        auto trimmed = trim_left(args);
        if (trimmed.empty()) {
            return CommandResult{"Usage: /resume <session-id>\nRestart with: cpp-harness --resume <path-to-session.jsonl>"};
        }
        std::string text;
        text += "To resume session '";
        text += trimmed;
        text += "', restart with: cpp-harness --resume <path-to-session.jsonl>";
        return CommandResult{std::move(text)};
    });
}

PromptProcessingResult process_prompt(
    std::string_view raw_input,
    const std::vector<PromptTemplate>& templates,
    CommandRegistry& registry,
    const CommandContext& ctx) {
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

        // Dispatch to built-in command registry
        auto cmd_result = registry.dispatch(name, ctx, args);
        if (cmd_result) {
            result.command_handled = true;
            result.display_text = std::move(cmd_result->display_text);
            result.shutdown_requested = cmd_result->shutdown_requested;
            return result;
        }

        // No built-in command matched — try template expansion
        auto expanded = expand_prompt_template(trimmed, templates);
        if (expanded != trimmed) {
            // Template matched and expanded — pass to agent loop
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

std::string expand_skill_command(
    std::string_view input,
    const std::vector<Skill>& skills,
    const harness::WorkspaceFileSystem& /*fs*/) {
    auto result = expand_skill_command_silent(input, skills);
    for (const auto& diag : result.diagnostics) {
        std::cerr << diag << '\n';
    }
    return std::move(result.expanded);
}

SkillExpansionResult expand_skill_command_silent(
    std::string_view input,
    const std::vector<Skill>& skills) {
    // Fast path: not a skill command
    if (!input.starts_with("/skill:")) {
        return {std::string{input}, {}};
    }

    // Parse skill name: text between /skill: and first space (or end of string)
    std::string_view rest = input.substr(7); // strip "/skill:"
    auto space_pos = rest.find_first_of(" \t");
    std::string_view name = (space_pos == std::string_view::npos)
        ? rest
        : rest.substr(0, space_pos);

    // Bare /skill: with no name — passthrough
    if (name.empty()) {
        return {std::string{input}, {}};
    }

    // Parse args: everything after the skill name
    std::string_view args = (space_pos == std::string_view::npos)
        ? std::string_view{}
        : trim_left(rest.substr(space_pos + 1));

    // Look up skill by name
    const Skill* found = nullptr;
    for (const auto& skill : skills) {
        if (skill.name == name) {
            found = &skill;
            break;
        }
    }

    if (!found) {
        std::vector<std::string> diags;
        diags.push_back(std::string{"[skill:warn] unknown skill: "} + std::string{name});
        return {std::string{input}, std::move(diags)};
    }

    // Use the cached skill content (already body after frontmatter from loader).
    return {formatSkillInvocation(*found, found->content, args), {}};
}

PromptProcessingResult process_prompt(
    std::string_view raw_input,
    const std::vector<PromptTemplate>& templates,
    CommandRegistry& registry,
    const CommandContext& ctx,
    const std::vector<Skill>& skills,
    const harness::WorkspaceFileSystem& fs) {
    // Expand /skill:name inline before command dispatch
    auto expanded_skill = expand_skill_command(raw_input, skills, fs);
    if (expanded_skill != raw_input) {
        // Skill was expanded — pass to agent loop as regular prompt
        PromptProcessingResult result;
        result.command_handled = false;
        result.expanded_prompt = std::move(expanded_skill);
        return result;
    }

    // No skill expansion — fall through to normal processing
    return process_prompt(raw_input, templates, registry, ctx);
}

} // namespace cch::coding_agent

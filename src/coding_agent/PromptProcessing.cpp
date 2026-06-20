#include "../../include/cch/coding_agent/PromptProcessing.hpp"

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

} // namespace cch::coding_agent

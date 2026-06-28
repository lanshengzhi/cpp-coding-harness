#include "../../include/cch/coding_agent/CommandRegistry.hpp"

#include <string>
#include <string_view>

namespace cch::coding_agent {
namespace {

[[nodiscard]] std::string_view trim_left(std::string_view sv) {
    while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t')) {
        sv.remove_prefix(1);
    }
    return sv;
}

} // namespace

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

} // namespace cch::coding_agent

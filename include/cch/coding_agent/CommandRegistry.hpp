#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace cch::coding_agent {

/// Context passed to command handlers.
struct CommandContext {
    /// Current session id (empty if no active session).
    std::string session_id;
    /// Current workspace path.
    std::string workspace_path;
    /// Current provider name.
    std::string provider;
    /// Current model name.
    std::string model;
    /// Number of messages in the current history.
    std::size_t message_count{0};
};

/// Result returned by a command handler.
struct CommandResult {
    /// Text to display to the user.
    std::string display_text;
    /// True if the session should shut down after this command.
    bool shutdown_requested{false};
};

/// Handler signature for slash-commands.
using CommandHandler = std::move_only_function<CommandResult(const CommandContext&, std::string_view args)>;

/// Registry mapping command names to handlers.
class CommandRegistry {
public:
    /// Returns a singleton empty registry (no commands registered).
    static CommandRegistry& empty() {
        static CommandRegistry instance;
        return instance;
    }

    /// Register a command handler.
    void register_command(std::string name, CommandHandler handler) {
        handlers_.emplace(std::move(name), std::move(handler));
    }

    /// Dispatch a command by name. Returns std::nullopt if not found.
    [[nodiscard]] std::optional<CommandResult> dispatch(
        std::string_view name,
        const CommandContext& ctx,
        std::string_view args) {
        auto it = handlers_.find(std::string{name});
        if (it == handlers_.end()) return std::nullopt;
        return it->second(ctx, args);
    }

    /// True if no commands registered.
    [[nodiscard]] bool is_empty() const { return handlers_.empty(); }

private:
    std::unordered_map<std::string, CommandHandler> handlers_;
};

/// Registers the built-in session-lifecycle slash commands.
void register_builtin_commands(CommandRegistry& registry);

} // namespace cch::coding_agent

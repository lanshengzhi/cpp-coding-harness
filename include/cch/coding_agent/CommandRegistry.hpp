#pragma once

#include <cch/util/Error.hpp>

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

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

/// Passive metadata describing an effective slash-command name.
struct CommandInfo {
    /// Command name without a leading slash.
    std::string name;
    std::string description;
    std::string argument_hint;
    /// Canonical command name when this entry is an alias.
    std::optional<std::string> alias_for;
};

/// Handler signature for slash-commands.
using CommandHandler = std::move_only_function<CommandResult(const CommandContext&, std::string_view args)>;

/// Registry owning command metadata and move-only handlers.
class CommandRegistry {
public:
    /// Returns a singleton empty registry (no commands registered).
    static CommandRegistry& empty() {
        static CommandRegistry instance;
        return instance;
    }

    /// Register a canonical command and its metadata.
    [[nodiscard]] util::ExpectedVoid register_command(
        std::string name,
        std::string description,
        std::string argument_hint,
        CommandHandler handler);

    /// Register a canonical command with empty description and argument hint.
    [[nodiscard]] util::ExpectedVoid register_command(std::string name, CommandHandler handler);

    /// Return canonical command metadata sorted lexicographically by name.
    [[nodiscard]] std::vector<CommandInfo> list_commands() const;

    /// Find canonical command metadata by name.
    [[nodiscard]] std::optional<CommandInfo> find_command_info(std::string_view name) const;

    /// Dispatch a canonical command by name. Returns std::nullopt if not found.
    [[nodiscard]] std::optional<CommandResult> dispatch(
        std::string_view name,
        const CommandContext& ctx,
        std::string_view args) {
        auto it = entries_.find(std::string{name});
        if (it == entries_.end()) return std::nullopt;
        return it->second.handler(ctx, args);
    }

    /// True if no commands registered.
    [[nodiscard]] bool is_empty() const { return entries_.empty(); }

private:
    struct Entry {
        CommandInfo info;
        CommandHandler handler;
    };

    std::unordered_map<std::string, Entry> entries_;
};

/// Registers the built-in session-lifecycle slash commands.
[[nodiscard]] util::ExpectedVoid register_builtin_commands(CommandRegistry& registry);

} // namespace cch::coding_agent

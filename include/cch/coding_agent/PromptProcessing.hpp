#pragma once

#include <cch/coding_agent/Skill.hpp>

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace cch::harness {
class WorkspaceFileSystem;
}

namespace cch::coding_agent {

/// Passive-value prompt template definition.
struct PromptTemplate {
    std::string name;
    std::optional<std::string> description;
    std::string content;
    /// Optional argument hint for autocomplete display (stored for future TUI use).
    std::optional<std::string> argument_hint = std::nullopt;
};

/// Result of processing user input through the command/template pipeline.
struct PromptProcessingResult {
    /// True if a slash-command consumed the input (no agent loop activation).
    bool command_handled{false};
    /// Message to display to the user when command_handled is true.
    std::optional<std::string> display_text;
    /// Text to pass to the agent loop (empty if command_handled).
    std::string expanded_prompt;
    /// True if the user requested shutdown (/quit).
    bool shutdown_requested{false};
};

// ── Input parsing helpers (shared between command dispatch and template expansion) ──

[[nodiscard]] inline std::string_view trim_left(std::string_view sv) {
    while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t')) {
        sv.remove_prefix(1);
    }
    return sv;
}

[[nodiscard]] inline std::string_view extract_command_name(std::string_view input) {
    auto trimmed = trim_left(input);
    if (trimmed.empty() || trimmed.front() != '/') return {};
    trimmed.remove_prefix(1); // strip '/'
    auto end = trimmed.find_first_of(" \t\n\r");
    return trimmed.substr(0, end);
}

[[nodiscard]] inline std::string_view extract_args(std::string_view input) {
    auto trimmed = trim_left(input);
    if (trimmed.empty() || trimmed.front() != '/') return {};
    trimmed.remove_prefix(1);
    auto space = trimmed.find_first_of(" \t");
    if (space == std::string_view::npos) return {};
    return trim_left(trimmed.substr(space + 1));
}

// ── Command dispatch types ──

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

/// Expand a skill command (/skill:name args) to its full <skill> XML block.
/// Returns the expanded text, or the original input if no skill matched.
/// Prints diagnostics to stderr for unknown skills and file read failures.
[[nodiscard]] std::string expand_skill_command(
    std::string_view input,
    const std::vector<Skill>& skills,
    const harness::WorkspaceFileSystem& fs);

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

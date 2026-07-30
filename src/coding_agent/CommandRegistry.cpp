#include "coding_agent/CommandRegistry.hpp"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::coding_agent {
namespace {

[[nodiscard]] util::ExpectedVoid validate_command_name(std::string_view name, std::string_view kind) {
    if (name.empty()) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            std::string{kind} + " name must not be empty"));
    }
    if (name.front() == '/') {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            std::string{kind} + " name must not start with '/'",
            {},
            std::string{name}));
    }
    if (std::any_of(name.begin(), name.end(), [](unsigned char ch) { return std::isspace(ch) != 0; })) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            std::string{kind} + " name must not contain whitespace",
            {},
            std::string{name}));
    }
    return {};
}

[[nodiscard]] CommandInfo make_alias_info(
    std::string_view alias,
    std::string_view canonical_name,
    const CommandInfo& canonical) {
    return CommandInfo{
        .name = std::string{alias},
        .description = canonical.description,
        .argument_hint = canonical.argument_hint,
        .alias_for = std::string{canonical_name},
    };
}

[[nodiscard]] std::vector<std::string_view> split_arguments(std::string_view args) {
    std::vector<std::string_view> result;
    while (!args.empty()) {
        while (!args.empty() && std::isspace(static_cast<unsigned char>(args.front())) != 0) {
            args.remove_prefix(1);
        }
        if (args.empty()) break;
        const auto end = args.find_first_of(" \t\n\r");
        result.push_back(args.substr(0, end));
        if (end == std::string_view::npos) break;
        args.remove_prefix(end);
    }
    return result;
}

[[nodiscard]] std::string command_usage(const CommandInfo& info) {
    std::string usage = "/" + info.name;
    if (!info.argument_hint.empty()) {
        usage += " " + info.argument_hint;
    }
    return usage;
}

[[nodiscard]] CommandResult help_command(const CommandContext& ctx, std::string_view args) {
    const auto arguments = split_arguments(args);
    if (arguments.size() > 1) {
        return CommandResult{"Usage: /help [command]"};
    }

    if (arguments.empty()) {
        std::vector<const CommandInfo*> sorted_commands;
        sorted_commands.reserve(ctx.available_commands.size());
        for (const auto& command : ctx.available_commands) {
            sorted_commands.push_back(&command);
        }
        std::sort(sorted_commands.begin(), sorted_commands.end(), [](const CommandInfo* lhs, const CommandInfo* rhs) {
            return lhs->name < rhs->name;
        });

        std::ostringstream text;
        text << "Available commands:";
        for (const auto* command : sorted_commands) {
            text << '\n';
            std::string invocation = "/" + command->name;
            std::string description = command->description;
            if (command->alias_for) {
                description = "Alias for /" + *command->alias_for;
            } else if (!command->argument_hint.empty()) {
                invocation += " " + command->argument_hint;
            }
            text << "  " << invocation;
            if (!description.empty()) {
                text << std::left << std::setw(24 - std::min<std::size_t>(invocation.size(), 23)) << ""
                     << description;
            }
        }
        if (ctx.user_bash_available) {
            // User Bash is input-prefix syntax, never a slash command or hotkey.
            text << "\nInput prefixes:"
                 << "\n  ! <command>           Run a shell command directly; the completed result enters later model context"
                 << "\n  !! <command>          Run a shell command directly; the completed result is excluded from later model context";
        }
        return CommandResult{text.str()};
    }

    auto name = arguments.front();
    if (!name.empty() && name.front() == '/') {
        name.remove_prefix(1);
    }
    const auto command = std::find_if(
        ctx.available_commands.begin(),
        ctx.available_commands.end(),
        [name](const CommandInfo& candidate) { return candidate.name == name; });
    if (command == ctx.available_commands.end()) {
        return CommandResult{"Unknown command: /" + std::string{name}};
    }

    std::string text = "Command: /" + command->name;
    text += "\nDescription: " + command->description;
    text += "\nUsage: " + command_usage(*command);
    if (command->alias_for) {
        text += "\nAlias for: /" + *command->alias_for;
    }
    return CommandResult{std::move(text)};
}

} // namespace

util::ExpectedVoid CommandRegistry::register_command(
    std::string name,
    std::string description,
    std::string argument_hint,
    CommandHandler handler) {
    if (auto valid_name = validate_command_name(name, "command"); !valid_name) {
        return std::unexpected(valid_name.error());
    }
    if (!handler) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "command handler must not be empty",
            {},
            name));
    }
    if (entries_.contains(name) || aliases_.contains(name)) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "duplicate command name: '" + name + "'",
            {},
            name));
    }

    auto key = name;
    entries_.emplace(
        std::move(key),
        Entry{
            .info = CommandInfo{
                .name = std::move(name),
                .description = std::move(description),
                .argument_hint = std::move(argument_hint),
                .alias_for = std::nullopt,
            },
            .handler = std::move(handler),
        });
    return {};
}

util::ExpectedVoid CommandRegistry::register_command(std::string name, CommandHandler handler) {
    return register_command(std::move(name), {}, {}, std::move(handler));
}

util::ExpectedVoid CommandRegistry::register_alias(std::string alias, std::string canonical_target) {
    if (auto valid_name = validate_command_name(alias, "alias"); !valid_name) {
        return std::unexpected(valid_name.error());
    }
    if (aliases_.contains(canonical_target)) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "alias target must not be another alias: '" + canonical_target + "'",
            {},
            canonical_target));
    }
    if (!entries_.contains(canonical_target)) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "alias target must be an existing canonical command: '" + canonical_target + "'",
            {},
            canonical_target));
    }
    if (entries_.contains(alias) || aliases_.contains(alias)) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "duplicate command name: '" + alias + "'",
            {},
            alias));
    }

    aliases_.emplace(std::move(alias), std::move(canonical_target));
    return {};
}

std::vector<CommandInfo> CommandRegistry::list_commands() const {
    std::vector<CommandInfo> commands;
    commands.reserve(entries_.size() + aliases_.size());
    for (const auto& [name, entry] : entries_) {
        (void)name;
        commands.push_back(entry.info);
    }
    for (const auto& [alias, canonical_name] : aliases_) {
        commands.push_back(make_alias_info(alias, canonical_name, entries_.at(canonical_name).info));
    }
    std::sort(commands.begin(), commands.end(), [](const CommandInfo& lhs, const CommandInfo& rhs) {
        return lhs.name < rhs.name;
    });
    return commands;
}

std::optional<CommandInfo> CommandRegistry::find_command_info(std::string_view name) const {
    const auto it = entries_.find(std::string{name});
    if (it != entries_.end()) return it->second.info;

    const auto alias = aliases_.find(std::string{name});
    if (alias == aliases_.end()) return std::nullopt;
    return make_alias_info(alias->first, alias->second, entries_.at(alias->second).info);
}

util::ExpectedVoid register_builtin_commands(CommandRegistry& registry) {
    // /session — print current session info
    if (auto registered = registry.register_command(
            "session",
            "Show current session information",
            {},
            [](const CommandContext& ctx, std::string_view /*args*/) {
                std::string text;
                text += "Session: " + ctx.session_id + "\n";
                // In-memory sessions have no file; name that state explicitly
                // instead of printing an ambiguous empty path.
                text += "File: " + ctx.session_path.value_or("In-memory") + "\n";
                text += "Workspace: " + ctx.workspace_path + "\n";
                text += "Provider: " + ctx.provider + "\n";
                text += "Model: " + ctx.model + "\n";
                text += "Messages: " + std::to_string(ctx.message_count);
                return CommandResult{std::move(text)};
            });
        !registered) {
        return std::unexpected(registered.error());
    }

    // /quit — signal shutdown
    if (auto registered = registry.register_command(
            "quit",
            "Quit the session",
            {},
            [](const CommandContext& /*ctx*/, std::string_view /*args*/) {
                return CommandResult{
                    .display_text = "Shutting down.",
                    .effect = CommandEffect::Shutdown,
                };
            });
        !registered) {
        return std::unexpected(registered.error());
    }

    if (auto registered = registry.register_alias("exit", "quit"); !registered) {
        return std::unexpected(registered.error());
    }

    // /clear — each concrete frontend applies the returned terminal effect.
    if (auto registered = registry.register_command(
            "clear",
            "Clear the terminal screen",
            {},
            [](const CommandContext& /*ctx*/, std::string_view args) {
                if (!args.empty()) {
                    return CommandResult{.display_text = "Usage: /clear"};
                }
                return CommandResult{
                    .display_text = {},
                    .effect = CommandEffect::ClearScreen,
                };
            });
        !registered) {
        return std::unexpected(registered.error());
    }

    if (auto registered = registry.register_command(
            "help",
            "Show available commands or help for one command",
            "[command]",
            help_command);
        !registered) {
        return std::unexpected(registered.error());
    }

    if (auto registered = registry.register_alias("commands", "help"); !registered) {
        return std::unexpected(registered.error());
    }

    return {};
}

util::ExpectedVoid register_native_tui_commands(CommandRegistry& registry) {
    if (auto registered = registry.register_command(
            "settings",
            "Open settings menu",
            {},
            [](const CommandContext& /*ctx*/, std::string_view args) {
                if (!args.empty()) {
                    return CommandResult{.display_text = "Usage: /settings"};
                }
                return CommandResult{
                    .display_text = {},
                    .effect = CommandEffect::OpenSettings,
                };
            });
        !registered) {
        return std::unexpected(registered.error());
    }

    if (auto registered = registry.register_command(
            "hotkeys",
            "Show all keyboard shortcuts",
            {},
            [](const CommandContext& /*ctx*/, std::string_view args) {
                if (!args.empty()) {
                    return CommandResult{.display_text = "Usage: /hotkeys"};
                }
                return CommandResult{
                    .display_text = {},
                    .effect = CommandEffect::OpenHotkeys,
                };
            });
        !registered) {
        return std::unexpected(registered.error());
    }

    return {};
}

} // namespace cch::coding_agent

#include "../../include/cch/coding_agent/CommandRegistry.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <utility>

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

[[nodiscard]] std::string_view trim_left(std::string_view sv) {
    while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t')) {
        sv.remove_prefix(1);
    }
    return sv;
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
    if (auto registered = registry.register_command("session", [](const CommandContext& ctx, std::string_view /*args*/) {
            std::string text;
            text += "Session: " + ctx.session_id + "\n";
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
    if (auto registered = registry.register_command("quit", [](const CommandContext& /*ctx*/, std::string_view /*args*/) {
            return CommandResult{"Shutting down.", true};
        });
        !registered) {
        return std::unexpected(registered.error());
    }

    // /new — start a new session (returns instruction text)
    if (auto registered = registry.register_command("new", [](const CommandContext& /*ctx*/, std::string_view /*args*/) {
            return CommandResult{"To start a new session, restart cpp-harness without --resume."};
        });
        !registered) {
        return std::unexpected(registered.error());
    }

    // /resume <session-id> — resume a previous session
    if (auto registered = registry.register_command("resume", [](const CommandContext& /*ctx*/, std::string_view args) {
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
        !registered) {
        return std::unexpected(registered.error());
    }

    return {};
}

} // namespace cch::coding_agent

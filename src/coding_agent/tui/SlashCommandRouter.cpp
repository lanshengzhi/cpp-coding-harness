#include "SlashCommandRouter.hpp"

#include <cch/coding_agent/PromptTemplate.hpp>
#include <cch/coding_agent/Skill.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace cch::coding_agent::tui {
namespace {

enum class SlashArgumentMode {
    None,
    Optional,
};

struct SlashCommandDefinition {
    SlashCommandId command;
    std::string_view canonical_name;
    SlashArgumentMode argument_mode;
    bool immediate;
};

constexpr std::array<SlashCommandDefinition, 19> kCommandDefinitions{{
    {.command = SlashCommandId::Clear,
     .canonical_name = "clear",
     .argument_mode = SlashArgumentMode::None,
     .immediate = true},
    {.command = SlashCommandId::Quit,
     .canonical_name = "quit",
     .argument_mode = SlashArgumentMode::None,
     .immediate = true},
    {.command = SlashCommandId::Copy,
     .canonical_name = "copy",
     .argument_mode = SlashArgumentMode::None,
     .immediate = true},
    {.command = SlashCommandId::Session,
     .canonical_name = "session",
     .argument_mode = SlashArgumentMode::None,
     .immediate = true},
    {.command = SlashCommandId::Hotkeys,
     .canonical_name = "hotkeys",
     .argument_mode = SlashArgumentMode::None,
     .immediate = true},
    {.command = SlashCommandId::Settings,
     .canonical_name = "settings",
     .argument_mode = SlashArgumentMode::None,
     .immediate = true},
    {.command = SlashCommandId::Help,
     .canonical_name = "help",
     .argument_mode = SlashArgumentMode::None,
     .immediate = true},
    {.command = SlashCommandId::Model,
     .canonical_name = "model",
     .argument_mode = SlashArgumentMode::Optional,
     .immediate = false},
    {.command = SlashCommandId::Models,
     .canonical_name = "models",
     .argument_mode = SlashArgumentMode::None,
     .immediate = false},
    {.command = SlashCommandId::Thinking,
     .canonical_name = "thinking",
     .argument_mode = SlashArgumentMode::Optional,
     .immediate = false},
    {.command = SlashCommandId::Login,
     .canonical_name = "login",
     .argument_mode = SlashArgumentMode::Optional,
     .immediate = false},
    {.command = SlashCommandId::Logout,
     .canonical_name = "logout",
     .argument_mode = SlashArgumentMode::None,
     .immediate = false},
    {.command = SlashCommandId::Resume,
     .canonical_name = "resume",
     .argument_mode = SlashArgumentMode::None,
     .immediate = false},
    {.command = SlashCommandId::Fork,
     .canonical_name = "fork",
     .argument_mode = SlashArgumentMode::None,
     .immediate = false},
    {.command = SlashCommandId::Tree,
     .canonical_name = "tree",
     .argument_mode = SlashArgumentMode::None,
     .immediate = false},
    {.command = SlashCommandId::Reload,
     .canonical_name = "reload",
     .argument_mode = SlashArgumentMode::None,
     .immediate = false},
    {.command = SlashCommandId::Compact,
     .canonical_name = "compact",
     .argument_mode = SlashArgumentMode::Optional,
     .immediate = false},
    {.command = SlashCommandId::Name,
     .canonical_name = "name",
     .argument_mode = SlashArgumentMode::Optional,
     .immediate = true},
    {.command = SlashCommandId::Trust,
     .canonical_name = "trust",
     .argument_mode = SlashArgumentMode::None,
     .immediate = false},
}};

struct SlashCommandAlias {
    std::string_view spelling;
    SlashCommandId command;
};

constexpr std::array<SlashCommandAlias, 24> kCommandAliases{{
    {.spelling = "clear", .command = SlashCommandId::Clear},
    {.spelling = "new", .command = SlashCommandId::Clear},
    {.spelling = "quit", .command = SlashCommandId::Quit},
    {.spelling = "exit", .command = SlashCommandId::Quit},
    {.spelling = "q", .command = SlashCommandId::Quit},
    {.spelling = "copy", .command = SlashCommandId::Copy},
    {.spelling = "session", .command = SlashCommandId::Session},
    {.spelling = "hotkeys", .command = SlashCommandId::Hotkeys},
    {.spelling = "settings", .command = SlashCommandId::Settings},
    {.spelling = "help", .command = SlashCommandId::Help},
    {.spelling = "commands", .command = SlashCommandId::Help},
    {.spelling = "model", .command = SlashCommandId::Model},
    {.spelling = "models", .command = SlashCommandId::Models},
    {.spelling = "scoped-models", .command = SlashCommandId::Models},
    {.spelling = "thinking", .command = SlashCommandId::Thinking},
    {.spelling = "login", .command = SlashCommandId::Login},
    {.spelling = "logout", .command = SlashCommandId::Logout},
    {.spelling = "resume", .command = SlashCommandId::Resume},
    {.spelling = "fork", .command = SlashCommandId::Fork},
    {.spelling = "tree", .command = SlashCommandId::Tree},
    {.spelling = "reload", .command = SlashCommandId::Reload},
    {.spelling = "compact", .command = SlashCommandId::Compact},
    {.spelling = "name", .command = SlashCommandId::Name},
    {.spelling = "trust", .command = SlashCommandId::Trust},
}};

constexpr std::array<std::string_view, 7> kThinkingLevels{
    "off", "minimal", "low", "medium", "high", "xhigh", "max"};

[[nodiscard]] bool is_ascii_space(char value) noexcept {
    return std::isspace(static_cast<unsigned char>(value)) != 0;
}

[[nodiscard]] std::string_view trim_ascii(std::string_view text) noexcept {
    while (!text.empty() && is_ascii_space(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && is_ascii_space(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

[[nodiscard]] const SlashCommandAlias* find_alias(std::string_view spelling) noexcept {
    const auto match = std::find_if(
        kCommandAliases.begin(),
        kCommandAliases.end(),
        [spelling](const SlashCommandAlias& alias) {
            return alias.spelling == spelling;
        });
    return match == kCommandAliases.end() ? nullptr : &*match;
}

[[nodiscard]] const SlashCommandDefinition* find_definition(
    SlashCommandId command) noexcept {
    const auto match = std::find_if(
        kCommandDefinitions.begin(),
        kCommandDefinitions.end(),
        [command](const SlashCommandDefinition& definition) {
            return definition.command == command;
        });
    return match == kCommandDefinitions.end() ? nullptr : &*match;
}

struct SlashCommandParts {
    std::string_view spelling;
    std::string_view argument;
};

/// Split a trimmed slash submission into its command token and argument.
/// Callers only use this after confirming the submission begins with `/`.
[[nodiscard]] SlashCommandParts split_slash_command(std::string_view text) noexcept {
    const auto body = trim_ascii(trim_ascii(text).substr(1));
    const auto separator = body.find_first_of(" \t\n\r\f\v");
    return SlashCommandParts{
        .spelling = body.substr(0, separator),
        .argument = separator == std::string_view::npos
            ? std::string_view{}
            : trim_ascii(body.substr(separator + 1)),
    };
}

[[nodiscard]] bool is_valid_thinking_level(std::string_view level) noexcept {
    return std::find(kThinkingLevels.begin(), kThinkingLevels.end(), level) !=
        kThinkingLevels.end();
}

[[nodiscard]] SlashCommandRouteError unknown_command_error(
    std::string_view spelling) {
    return SlashCommandRouteError{
        .message = std::format("Unknown slash command '/{}'", spelling),
        .kind = SlashCommandRouteErrorKind::UnknownCommand,
    };
}

[[nodiscard]] SlashCommandRouteError invalid_arguments_error(
    std::string_view spelling) {
    return SlashCommandRouteError{
        .message = std::format(
            "Slash command '/{}' does not accept arguments",
            spelling),
        .kind = SlashCommandRouteErrorKind::Invalid,
    };
}

[[nodiscard]] SlashCommandRouteError invalid_thinking_level_error(
    std::string_view level) {
    return SlashCommandRouteError{
        .message = std::format(
            "Invalid thinking level '{}' (expected off, minimal, low, medium, high, xhigh, or max)",
            level),
        .kind = SlashCommandRouteErrorKind::Invalid,
    };
}

[[nodiscard]] SlashCommandRouteError execution_error(
    SlashCommandId command,
    const support::Error& error) {
    const auto detail = error.message.empty() ? "unknown error" : error.message;
    return SlashCommandRouteError{
        .message = std::format(
            "Could not execute /{}: {}",
            slash_command_name(command),
            detail),
        .kind = SlashCommandRouteErrorKind::Invalid,
    };
}

} // namespace

std::string_view slash_command_name(SlashCommandId command) noexcept {
    if (const auto* definition = find_definition(command)) {
        return definition->canonical_name;
    }
    return "unknown";
}

bool is_immediate_slash_command(SlashCommandId command) noexcept {
    const auto* definition = find_definition(command);
    return definition != nullptr && definition->immediate;
}

SlashCommandParseResultVariant SlashCommandRouter::parse(std::string_view text) {
    const auto trimmed = trim_ascii(text);
    if (!trimmed.starts_with('/')) {
        return SlashCommandPassThrough{};
    }

    const auto parts = split_slash_command(text);
    if (parts.spelling.empty()) {
        return SlashCommandRouteError{
            .message = "A slash command must include a command name",
            .kind = SlashCommandRouteErrorKind::Invalid,
        };
    }

    const auto* alias = find_alias(parts.spelling);
    if (alias == nullptr) {
        return unknown_command_error(parts.spelling);
    }
    const auto* definition = find_definition(alias->command);
    if (definition == nullptr) {
        return SlashCommandRouteError{
            .message = "Slash command metadata is unavailable",
            .kind = SlashCommandRouteErrorKind::Invalid,
        };
    }
    if (definition->argument_mode == SlashArgumentMode::None &&
        !parts.argument.empty()) {
        return invalid_arguments_error(parts.spelling);
    }
    if (alias->command == SlashCommandId::Thinking &&
        !parts.argument.empty() && !is_valid_thinking_level(parts.argument)) {
        return invalid_thinking_level_error(parts.argument);
    }

    return SlashCommandInvocation{
        .command = alias->command,
        .argument = std::string{parts.argument},
    };
}

SlashCommandRouteVariant SlashCommandRouter::route(
    std::string_view text,
    SlashCommandExecutionContext& context) const {
    auto parsed = parse(text);
    if (auto* error = std::get_if<SlashCommandRouteError>(&parsed)) {
        if (error->kind == SlashCommandRouteErrorKind::UnknownCommand &&
            context.allow_unrecognized) {
            const auto parts = split_slash_command(text);
            if (context.allow_unrecognized(parts.spelling)) {
                return SlashCommandPassThrough{};
            }
        }
        return std::move(*error);
    }
    if (std::holds_alternative<SlashCommandPassThrough>(parsed)) {
        return SlashCommandPassThrough{};
    }

    auto invocation = std::move(std::get<SlashCommandInvocation>(parsed));
    if (is_immediate_slash_command(invocation.command)) {
        if (!context.execute_immediate) {
            return SlashCommandRouteError{
                .message = "Immediate slash command execution is unavailable",
                .kind = SlashCommandRouteErrorKind::Invalid,
            };
        }
        if (auto executed = context.execute_immediate(invocation); !executed) {
            return execution_error(invocation.command, executed.error());
        }
        return SlashCommandImmediateResult{
            .invocation = std::move(invocation),
        };
    }
    return SlashCommandModalResult{
        .invocation = std::move(invocation),
    };
}

bool is_dynamic_slash_command(
    std::string_view command,
    std::span<const coding_agent::PromptTemplate> prompt_templates,
    std::span<const coding_agent::Skill> skills,
    bool skill_commands_enabled) {
    for (const auto& prompt_template : prompt_templates) {
        if (prompt_template.name == command) return true;
    }
    if (!skill_commands_enabled || !command.starts_with("skill:")) {
        return false;
    }
    const auto skill_name = command.substr(std::string_view{"skill:"}.size());
    if (skill_name.empty()) return false;
    for (const auto& skill : skills) {
        if (skill.name == skill_name) return true;
    }
    return false;
}

} // namespace cch::coding_agent::tui

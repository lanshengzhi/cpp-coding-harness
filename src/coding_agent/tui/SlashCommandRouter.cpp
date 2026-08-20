#include "SlashCommandRouter.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <string>
#include <string_view>
#include <utility>

namespace cch::coding_agent::tui {
namespace {

enum class SlashArgumentMode {
    None,
    Optional,
};

struct SlashCommandAlias {
    std::string_view spelling;
    SlashCommandId command;
    SlashArgumentMode argument_mode;
};

constexpr std::array<SlashCommandAlias, 24> kCommandAliases{{
    {"clear", SlashCommandId::Clear, SlashArgumentMode::None},
    {"new", SlashCommandId::Clear, SlashArgumentMode::None},
    {"quit", SlashCommandId::Quit, SlashArgumentMode::None},
    {"exit", SlashCommandId::Quit, SlashArgumentMode::None},
    {"q", SlashCommandId::Quit, SlashArgumentMode::None},
    {"copy", SlashCommandId::Copy, SlashArgumentMode::None},
    {"session", SlashCommandId::Session, SlashArgumentMode::None},
    {"hotkeys", SlashCommandId::Hotkeys, SlashArgumentMode::None},
    {"settings", SlashCommandId::Settings, SlashArgumentMode::None},
    {"help", SlashCommandId::Help, SlashArgumentMode::None},
    {"commands", SlashCommandId::Help, SlashArgumentMode::None},
    {"model", SlashCommandId::Model, SlashArgumentMode::Optional},
    {"models", SlashCommandId::Models, SlashArgumentMode::None},
    {"scoped-models", SlashCommandId::Models, SlashArgumentMode::None},
    {"thinking", SlashCommandId::Thinking, SlashArgumentMode::Optional},
    {"login", SlashCommandId::Login, SlashArgumentMode::Optional},
    {"logout", SlashCommandId::Logout, SlashArgumentMode::None},
    {"resume", SlashCommandId::Resume, SlashArgumentMode::None},
    {"fork", SlashCommandId::Fork, SlashArgumentMode::None},
    {"tree", SlashCommandId::Tree, SlashArgumentMode::None},
    {"reload", SlashCommandId::Reload, SlashArgumentMode::None},
    {"compact", SlashCommandId::Compact, SlashArgumentMode::Optional},
    {"name", SlashCommandId::Name, SlashArgumentMode::Optional},
    {"trust", SlashCommandId::Trust, SlashArgumentMode::None},
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

[[nodiscard]] bool is_valid_thinking_level(std::string_view level) noexcept {
    return std::find(kThinkingLevels.begin(), kThinkingLevels.end(), level) !=
        kThinkingLevels.end();
}

[[nodiscard]] SlashCommandRouteError unknown_command_error(
    std::string_view spelling) {
    return SlashCommandRouteError{
        .message = "Unknown slash command '/" + std::string{spelling} + "'",
        .unknown_command = true,
    };
}

[[nodiscard]] SlashCommandRouteError invalid_arguments_error(
    std::string_view spelling) {
    return SlashCommandRouteError{
        .message = "Slash command '/" + std::string{spelling} +
            "' does not accept arguments",
        .unknown_command = false,
    };
}

[[nodiscard]] SlashCommandRouteError invalid_thinking_level_error(
    std::string_view level) {
    return SlashCommandRouteError{
        .message = "Invalid thinking level '" + std::string{level} +
            "' (expected off, minimal, low, medium, high, xhigh, or max)",
        .unknown_command = false,
    };
}

[[nodiscard]] SlashCommandRouteError execution_error(
    SlashCommandId command,
    const support::Error& error) {
    const auto detail = error.message.empty() ? "unknown error" : error.message;
    return SlashCommandRouteError{
        .message = "Could not execute /" + std::string{slash_command_name(command)} +
            ": " + detail,
        .unknown_command = false,
    };
}

} // namespace

std::string_view slash_command_name(SlashCommandId command) noexcept {
    switch (command) {
    case SlashCommandId::Clear:
        return "clear";
    case SlashCommandId::Quit:
        return "quit";
    case SlashCommandId::Copy:
        return "copy";
    case SlashCommandId::Session:
        return "session";
    case SlashCommandId::Hotkeys:
        return "hotkeys";
    case SlashCommandId::Settings:
        return "settings";
    case SlashCommandId::Help:
        return "help";
    case SlashCommandId::Model:
        return "model";
    case SlashCommandId::Models:
        return "models";
    case SlashCommandId::Thinking:
        return "thinking";
    case SlashCommandId::Login:
        return "login";
    case SlashCommandId::Logout:
        return "logout";
    case SlashCommandId::Resume:
        return "resume";
    case SlashCommandId::Fork:
        return "fork";
    case SlashCommandId::Tree:
        return "tree";
    case SlashCommandId::Reload:
        return "reload";
    case SlashCommandId::Compact:
        return "compact";
    case SlashCommandId::Name:
        return "name";
    case SlashCommandId::Trust:
        return "trust";
    }
    return "unknown";
}

bool is_immediate_slash_command(SlashCommandId command) noexcept {
    switch (command) {
    case SlashCommandId::Clear:
    case SlashCommandId::Quit:
    case SlashCommandId::Copy:
    case SlashCommandId::Session:
    case SlashCommandId::Hotkeys:
    case SlashCommandId::Settings:
    case SlashCommandId::Help:
    case SlashCommandId::Name:
        return true;
    case SlashCommandId::Model:
    case SlashCommandId::Models:
    case SlashCommandId::Thinking:
    case SlashCommandId::Login:
    case SlashCommandId::Logout:
    case SlashCommandId::Resume:
    case SlashCommandId::Fork:
    case SlashCommandId::Tree:
    case SlashCommandId::Reload:
    case SlashCommandId::Compact:
    case SlashCommandId::Trust:
        return false;
    }
    return false;
}

SlashCommandParseResult SlashCommandRouter::parse(std::string_view text) {
    const auto trimmed = trim_ascii(text);
    if (!trimmed.starts_with('/')) {
        return SlashCommandPassThrough{};
    }

    const auto body = trim_ascii(trimmed.substr(1));
    if (body.empty()) {
        return SlashCommandRouteError{
            .message = "A slash command must include a command name",
            .unknown_command = false,
        };
    }

    const auto separator = body.find_first_of(" \t\n\r\f\v");
    const auto spelling = body.substr(0, separator);
    const auto argument = separator == std::string_view::npos
        ? std::string_view{}
        : trim_ascii(body.substr(separator + 1));
    const auto* alias = find_alias(spelling);
    if (alias == nullptr) {
        return unknown_command_error(spelling);
    }
    if (alias->argument_mode == SlashArgumentMode::None && !argument.empty()) {
        return invalid_arguments_error(spelling);
    }
    if (alias->command == SlashCommandId::Thinking &&
        !argument.empty() && !is_valid_thinking_level(argument)) {
        return invalid_thinking_level_error(argument);
    }

    return SlashCommandInvocation{
        .command = alias->command,
        .argument = std::string{argument},
    };
}

SlashCommandRoute SlashCommandRouter::route(
    std::string_view text,
    SlashCommandExecutionContext& context) const {
    auto parsed = parse(text);
    if (auto* error = std::get_if<SlashCommandRouteError>(&parsed)) {
        if (error->unknown_command && context.allow_unrecognized) {
            const auto trimmed = trim_ascii(text);
            const auto body = trim_ascii(trimmed.substr(1));
            const auto separator = body.find_first_of(" \t\n\r\f\v");
            const auto spelling = body.substr(0, separator);
            if (context.allow_unrecognized(spelling)) {
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
                .unknown_command = false,
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

} // namespace cch::coding_agent::tui

#include "../../third_party/catch2/catch_test_macros.hpp"

#include "coding_agent/CommandRegistry.hpp"

#include <string>
#include <utility>
#include <variant>

using namespace cch;

TEST_CASE("built-in /session command returns session info", "[coding_agent][prompt]") {
    coding_agent::CommandRegistry registry;
    REQUIRE(coding_agent::register_builtin_commands(registry).has_value());

    coding_agent::CommandContext ctx{
        .session_id = "test-session-1",
        .session_path = "/tmp/ws/session.jsonl",
        .workspace_path = "/tmp/ws",
        .provider = "openai",
        .model = "gpt-4.1-mini",
        .message_count = 5,
        .available_commands = {},
    };

    auto result = registry.dispatch("session", ctx, "");
    REQUIRE(result.has_value());
    CHECK(result->display_text.find("test-session-1") != std::string::npos);
    CHECK(result->display_text.find("/tmp/ws") != std::string::npos);
    CHECK(result->display_text.find("openai") != std::string::npos);
    CHECK(result->display_text.find("gpt-4.1-mini") != std::string::npos);
    CHECK(result->display_text.find("5") != std::string::npos);
    CHECK(result->display_text.find("File: /tmp/ws/session.jsonl") != std::string::npos);
    CHECK(result->display_text.find("In-memory") == std::string::npos);
}

TEST_CASE("built-in /session command identifies an in-memory session explicitly", "[coding_agent][prompt]") {
    coding_agent::CommandRegistry registry;
    REQUIRE(coding_agent::register_builtin_commands(registry).has_value());

    coding_agent::CommandContext ctx{
        .session_id = "test-session-mem",
        .workspace_path = "/tmp/ws",
        .provider = "openai",
        .model = "gpt-4.1-mini",
        .message_count = 0,
        .available_commands = {},
    };

    auto result = registry.dispatch("session", ctx, "");
    REQUIRE(result.has_value());
    // The absent path must never surface as an ambiguous empty value.
    CHECK(result->display_text.find("File: In-memory") != std::string::npos);
    CHECK(result->display_text.find("File: \n") == std::string::npos);
}

TEST_CASE("built-in /quit and /exit commands signal shutdown through the same handler", "[coding_agent][prompt]") {
    coding_agent::CommandRegistry registry;
    REQUIRE(coding_agent::register_builtin_commands(registry).has_value());

    coding_agent::CommandContext ctx;
    auto quit = registry.dispatch("quit", ctx, "");
    auto exit = registry.dispatch("exit", ctx, "");

    REQUIRE(quit.has_value());
    REQUIRE(exit.has_value());
    CHECK(quit->effect == coding_agent::CommandEffect::Shutdown);
    CHECK(quit->display_text == "Shutting down.");
    CHECK(exit->effect == coding_agent::CommandEffect::Shutdown);
    CHECK(exit->display_text == quit->display_text);
}

TEST_CASE("built-in /clear returns one concrete frontend effect", "[coding_agent][prompt]") {
    coding_agent::CommandRegistry registry;
    REQUIRE(coding_agent::register_builtin_commands(registry).has_value());

    coding_agent::CommandContext ctx;
    auto clear = registry.dispatch("clear", ctx, "");
    REQUIRE(clear.has_value());
    CHECK(clear->effect == coding_agent::CommandEffect::ClearScreen);
    CHECK(clear->display_text.empty());

    auto with_arguments = registry.dispatch("clear", ctx, "now");
    REQUIRE(with_arguments.has_value());
    CHECK(with_arguments->effect == coding_agent::CommandEffect::None);
    CHECK(with_arguments->display_text == "Usage: /clear");
}

TEST_CASE("built-in command metadata describes the implemented operations", "[coding_agent][prompt]") {
    coding_agent::CommandRegistry registry;
    REQUIRE(coding_agent::register_builtin_commands(registry).has_value());

    const auto commands = registry.list_commands();
    REQUIRE(commands.size() == 6);
    CHECK(commands[0].name == "clear");
    CHECK(commands[0].description == "Clear the terminal screen");
    CHECK(commands[1].name == "commands");
    CHECK(commands[1].alias_for == "help");
    CHECK(commands[2].name == "exit");
    CHECK(commands[2].alias_for == "quit");
    CHECK(commands[2].description == "Quit the session");
    CHECK(commands[3].name == "help");
    CHECK(commands[3].description == "Show available commands or help for one command");
    CHECK(commands[3].argument_hint == "[command]");
    CHECK(commands[4].name == "quit");
    CHECK(commands[4].description == "Quit the session");
    CHECK(commands[5].name == "session");
    CHECK(commands[5].description == "Show current session information");
}

TEST_CASE("built-in /help lists the passive command snapshot deterministically", "[coding_agent][prompt]") {
    coding_agent::CommandRegistry registry;
    REQUIRE(coding_agent::register_builtin_commands(registry).has_value());
    REQUIRE(registry.register_command("undocumented", [](const coding_agent::CommandContext&, std::string_view) {
        return coding_agent::CommandResult{};
    }).has_value());

    coding_agent::CommandContext ctx;
    ctx.available_commands = registry.list_commands();
    auto result = registry.dispatch("help", ctx, "");

    REQUIRE(result.has_value());
    CHECK(result->display_text ==
          "Available commands:\n"
          "  /clear                  Clear the terminal screen\n"
          "  /commands               Alias for /help\n"
          "  /exit                   Alias for /quit\n"
          "  /help [command]         Show available commands or help for one command\n"
          "  /quit                   Quit the session\n"
          "  /session                Show current session information\n"
          "  /undocumented");
}

TEST_CASE("built-in /help shows canonical and alias details from passive metadata", "[coding_agent][prompt]") {
    coding_agent::CommandRegistry registry;
    REQUIRE(coding_agent::register_builtin_commands(registry).has_value());

    coding_agent::CommandContext ctx;
    ctx.available_commands = registry.list_commands();

    auto canonical = registry.dispatch("help", ctx, "help");
    REQUIRE(canonical.has_value());
    CHECK(canonical->display_text ==
          "Command: /help\n"
          "Description: Show available commands or help for one command\n"
          "Usage: /help [command]");

    auto slash_prefixed = registry.dispatch("help", ctx, "/session");
    REQUIRE(slash_prefixed.has_value());
    CHECK(slash_prefixed->display_text ==
          "Command: /session\n"
          "Description: Show current session information\n"
          "Usage: /session");

    auto alias = registry.dispatch("help", ctx, "/commands");
    REQUIRE(alias.has_value());
    CHECK(alias->display_text ==
          "Command: /commands\n"
          "Description: Show available commands or help for one command\n"
          "Usage: /commands [command]\n"
          "Alias for: /help");
}

TEST_CASE("built-in /help handles invalid arity and unknown detailed targets", "[coding_agent][prompt]") {
    coding_agent::CommandRegistry registry;
    REQUIRE(coding_agent::register_builtin_commands(registry).has_value());

    coding_agent::CommandContext ctx;
    ctx.available_commands = registry.list_commands();

    auto invalid_arity = registry.dispatch("help", ctx, "session extra");
    REQUIRE(invalid_arity.has_value());
    CHECK(invalid_arity->display_text == "Usage: /help [command]");

    auto unknown = registry.dispatch("help", ctx, "missing");
    REQUIRE(unknown.has_value());
    CHECK(unknown->display_text == "Unknown command: /missing");
}

TEST_CASE("Native TUI commands extend only the concrete effective registry", "[coding_agent][prompt][issue60]") {
    coding_agent::CommandRegistry registry;
    REQUIRE(coding_agent::register_builtin_commands(registry).has_value());
    REQUIRE(coding_agent::register_native_tui_commands(registry).has_value());

    const auto commands = registry.list_commands();
    REQUIRE(commands.size() == 10);
    CHECK(commands[4].name == "hotkeys");
    CHECK(commands[4].description == "Show all keyboard shortcuts");
    CHECK(commands[9].name == "settings");
    CHECK(commands[9].description == "Open settings menu");
    CHECK(commands[5].name == "login");
    CHECK(commands[5].description == "Configure provider authentication");
    CHECK(commands[5].argument_hint == "<provider>");
    CHECK(commands[6].name == "logout");
    CHECK(commands[6].description == "Remove provider authentication");
    CHECK_FALSE(registry.find_command_info("new").has_value());
    CHECK_FALSE(registry.find_command_info("resume").has_value());

    const coding_agent::CommandContext context;
    const auto settings = registry.dispatch("settings", context, "");
    const auto hotkeys = registry.dispatch("hotkeys", context, "");
    const auto login = registry.dispatch("login", context, "deepseek");
    const auto logout = registry.dispatch("logout", context, "");
    REQUIRE(settings.has_value());
    REQUIRE(hotkeys.has_value());
    REQUIRE(login.has_value());
    REQUIRE(logout.has_value());
    CHECK(settings->effect == coding_agent::CommandEffect::OpenSettings);
    CHECK(hotkeys->effect == coding_agent::CommandEffect::OpenHotkeys);
    CHECK(login->effect == coding_agent::CommandEffect::OpenLogin);
    CHECK(login->effect_argument == "deepseek");
    CHECK(logout->effect == coding_agent::CommandEffect::OpenLogout);
}

TEST_CASE("built-in /commands dispatches through the /help handler", "[coding_agent][prompt]") {
    coding_agent::CommandRegistry registry;
    REQUIRE(coding_agent::register_builtin_commands(registry).has_value());

    coding_agent::CommandContext ctx;
    ctx.available_commands = registry.list_commands();
    auto help = registry.dispatch("help", ctx, "session");
    auto commands = registry.dispatch("commands", ctx, "session");

    REQUIRE(help.has_value());
    REQUIRE(commands.has_value());
    CHECK(commands->display_text == help->display_text);
}

TEST_CASE("built-in /help documents input prefixes only where User Bash is available", "[coding_agent][prompt][issue89]") {
    coding_agent::CommandRegistry registry;
    REQUIRE(coding_agent::register_builtin_commands(registry).has_value());

    coding_agent::CommandContext ctx;
    ctx.available_commands = registry.list_commands();
    ctx.user_bash_available = true;
    auto with_bash = registry.dispatch("help", ctx, "");
    REQUIRE(with_bash.has_value());
    CHECK(with_bash->display_text.find("Available commands:") != std::string::npos);
    CHECK(with_bash->display_text.find("Input prefixes:") != std::string::npos);
    CHECK(with_bash->display_text.find("! <command>") != std::string::npos);
    CHECK(with_bash->display_text.find("!! <command>") != std::string::npos);

    ctx.user_bash_available = false;
    auto without_bash = registry.dispatch("help", ctx, "");
    REQUIRE(without_bash.has_value());
    CHECK(without_bash->display_text.find("Available commands:") != std::string::npos);
    CHECK(without_bash->display_text.find("Input prefixes:") == std::string::npos);

    // The prefixes are input prefixes, not registered slash commands.
    auto pseudo = registry.dispatch("help", ctx, "!");
    REQUIRE(pseudo.has_value());
    CHECK(pseudo->display_text == "Unknown command: /!");
}

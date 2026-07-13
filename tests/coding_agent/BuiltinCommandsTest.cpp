#include "../../third_party/catch2/catch_test_macros.hpp"

#include "../../include/cch/coding_agent/PromptProcessing.hpp"

using namespace cch;

TEST_CASE("process_prompt passes through normal text unchanged", "[coding_agent][prompt]") {
    coding_agent::CommandRegistry registry;
    auto result = coding_agent::process_prompt("hello world", {}, registry);
    CHECK(result.command_handled == false);
    CHECK(result.expanded_prompt == "hello world");
    CHECK_FALSE(result.shutdown_requested);
}

TEST_CASE("process_prompt detects slash command with no name", "[coding_agent][prompt]") {
    coding_agent::CommandRegistry registry;
    auto result = coding_agent::process_prompt("/", {}, registry);
    CHECK(result.command_handled == true);
    REQUIRE(result.display_text);
    CHECK(result.display_text->find("command name") != std::string::npos);
}

TEST_CASE("process_prompt returns error for unknown command", "[coding_agent][prompt]") {
    coding_agent::CommandRegistry registry;
    auto result = coding_agent::process_prompt("/nonexistent", {}, registry);
    CHECK(result.command_handled == true);
    REQUIRE(result.display_text);
    CHECK(result.display_text->find("Unknown command") != std::string::npos);
}

TEST_CASE("process_prompt detects shell passthrough prefix", "[coding_agent][prompt]") {
    coding_agent::CommandRegistry registry;
    auto result = coding_agent::process_prompt("!echo hello", {}, registry);
    CHECK(result.command_handled == true);
    REQUIRE(result.display_text);
    CHECK(result.display_text->find("not yet implemented") != std::string::npos);
}

TEST_CASE("built-in /session command returns session info", "[coding_agent][prompt]") {
    coding_agent::CommandRegistry registry;
    REQUIRE(coding_agent::register_builtin_commands(registry).has_value());

    coding_agent::CommandContext ctx{
        .session_id = "test-session-1",
        .workspace_path = "/tmp/ws",
        .provider = "openai",
        .model = "gpt-4.1-mini",
        .message_count = 5,
        .available_commands = {},
    };

    auto result = coding_agent::process_prompt("/session", {}, registry, ctx);
    CHECK(result.command_handled == true);
    REQUIRE(result.display_text);
    CHECK(result.display_text->find("test-session-1") != std::string::npos);
    CHECK(result.display_text->find("/tmp/ws") != std::string::npos);
    CHECK(result.display_text->find("openai") != std::string::npos);
    CHECK(result.display_text->find("gpt-4.1-mini") != std::string::npos);
    CHECK(result.display_text->find("5") != std::string::npos);
}

TEST_CASE("built-in /quit and /exit commands signal shutdown through the same handler", "[coding_agent][prompt]") {
    coding_agent::CommandRegistry registry;
    REQUIRE(coding_agent::register_builtin_commands(registry).has_value());

    coding_agent::CommandContext ctx;
    auto quit = coding_agent::process_prompt("/quit", {}, registry, ctx);
    auto exit = coding_agent::process_prompt("/exit", {}, registry, ctx);

    CHECK(quit.command_handled == true);
    CHECK(quit.shutdown_requested == true);
    REQUIRE(quit.display_text);
    CHECK(*quit.display_text == "Shutting down.");
    CHECK(exit.command_handled == true);
    CHECK(exit.shutdown_requested == true);
    REQUIRE(exit.display_text);
    CHECK(*exit.display_text == *quit.display_text);
}

TEST_CASE("built-in /clear is a no-op command outside the text presentation seam", "[coding_agent][prompt]") {
    coding_agent::CommandRegistry registry;
    REQUIRE(coding_agent::register_builtin_commands(registry).has_value());

    coding_agent::CommandContext ctx;
    auto clear = coding_agent::process_prompt("/clear", {}, registry, ctx);
    CHECK(clear.command_handled == true);
    CHECK_FALSE(clear.shutdown_requested);
    REQUIRE(clear.display_text);
    CHECK(clear.display_text->empty());

    auto with_arguments = coding_agent::process_prompt("/clear now", {}, registry, ctx);
    CHECK(with_arguments.command_handled == true);
    CHECK_FALSE(with_arguments.shutdown_requested);
    REQUIRE(with_arguments.display_text);
    CHECK(*with_arguments.display_text == "Usage: /clear");
}

TEST_CASE("built-in /new command returns instruction text", "[coding_agent][prompt]") {
    coding_agent::CommandRegistry registry;
    REQUIRE(coding_agent::register_builtin_commands(registry).has_value());

    coding_agent::CommandContext ctx;
    auto result = coding_agent::process_prompt("/new", {}, registry, ctx);
    CHECK(result.command_handled == true);
    REQUIRE(result.display_text);
    CHECK(result.display_text->find("restart") != std::string::npos);
}

TEST_CASE("built-in /resume command with args returns instruction", "[coding_agent][prompt]") {
    coding_agent::CommandRegistry registry;
    REQUIRE(coding_agent::register_builtin_commands(registry).has_value());

    coding_agent::CommandContext ctx;
    auto result = coding_agent::process_prompt("/resume abc123", {}, registry, ctx);
    CHECK(result.command_handled == true);
    REQUIRE(result.display_text);
    CHECK(result.display_text->find("abc123") != std::string::npos);
    CHECK(result.display_text->find("--resume") != std::string::npos);
}

TEST_CASE("built-in /resume without args shows usage", "[coding_agent][prompt]") {
    coding_agent::CommandRegistry registry;
    REQUIRE(coding_agent::register_builtin_commands(registry).has_value());

    coding_agent::CommandContext ctx;
    auto result = coding_agent::process_prompt("/resume", {}, registry, ctx);
    CHECK(result.command_handled == true);
    REQUIRE(result.display_text);
    CHECK(result.display_text->find("Usage:") != std::string::npos);
}

TEST_CASE("built-in command metadata describes the implemented operations", "[coding_agent][prompt]") {
    coding_agent::CommandRegistry registry;
    REQUIRE(coding_agent::register_builtin_commands(registry).has_value());

    const auto commands = registry.list_commands();
    REQUIRE(commands.size() == 8);
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
    CHECK(commands[4].name == "new");
    CHECK(commands[4].description == "Show restart instructions for a new session");
    CHECK(commands[5].name == "quit");
    CHECK(commands[5].description == "Quit the session");
    CHECK(commands[6].name == "resume");
    CHECK(commands[6].description == "Show restart instructions for resuming a session");
    CHECK(commands[6].argument_hint == "<session-id>");
    CHECK(commands[7].name == "session");
    CHECK(commands[7].description == "Show current session information");
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
          "  /new                    Show restart instructions for a new session\n"
          "  /quit                   Quit the session\n"
          "  /resume <session-id>    Show restart instructions for resuming a session\n"
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

    auto invalid_arity = coding_agent::process_prompt("/help session extra", {}, registry, ctx);
    CHECK(invalid_arity.command_handled);
    REQUIRE(invalid_arity.display_text);
    CHECK(*invalid_arity.display_text == "Usage: /help [command]");

    auto unknown = coding_agent::process_prompt("/help missing", {}, registry, ctx);
    CHECK(unknown.command_handled);
    REQUIRE(unknown.display_text);
    CHECK(*unknown.display_text == "Unknown command: /missing");
}

TEST_CASE("built-in /commands dispatches through the /help handler", "[coding_agent][prompt]") {
    coding_agent::CommandRegistry registry;
    REQUIRE(coding_agent::register_builtin_commands(registry).has_value());

    coding_agent::CommandContext ctx;
    ctx.available_commands = registry.list_commands();
    auto help = coding_agent::process_prompt("/help session", {}, registry, ctx);
    auto commands = coding_agent::process_prompt("/commands session", {}, registry, ctx);

    REQUIRE(help.display_text);
    REQUIRE(commands.display_text);
    CHECK(*commands.display_text == *help.display_text);
}

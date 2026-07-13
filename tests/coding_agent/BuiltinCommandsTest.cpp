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

TEST_CASE("built-in /quit command signals shutdown", "[coding_agent][prompt]") {
    coding_agent::CommandRegistry registry;
    REQUIRE(coding_agent::register_builtin_commands(registry).has_value());

    coding_agent::CommandContext ctx;
    auto result = coding_agent::process_prompt("/quit", {}, registry, ctx);
    CHECK(result.command_handled == true);
    CHECK(result.shutdown_requested == true);
    REQUIRE(result.display_text);
    CHECK(result.display_text->find("Shutting down") != std::string::npos);
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

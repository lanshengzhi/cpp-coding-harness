#include "../../third_party/catch2/catch_test_macros.hpp"

#include "coding_agent/prompt/PromptProcessor.hpp"

#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>

using namespace cch;

TEST_CASE("prompt processor returns ordinary and empty input as agent prompts", "[coding_agent][prompt][processor]") {
    coding_agent::prompt::PromptProcessor processor{
        coding_agent::prompt::PromptResources{}};

    const auto ordinary = processor.process("hello", {});
    const auto empty = processor.process("", {});

    REQUIRE(std::holds_alternative<coding_agent::prompt::AgentPrompt>(ordinary));
    CHECK(std::get<coding_agent::prompt::AgentPrompt>(ordinary).text == "hello");
    REQUIRE(std::holds_alternative<coding_agent::prompt::AgentPrompt>(empty));
    CHECK(std::get<coding_agent::prompt::AgentPrompt>(empty).text.empty());
}

TEST_CASE("prompt processor dispatches commands only at column zero and preserves command arguments", "[coding_agent][prompt][processor]") {
    coding_agent::CommandRegistry commands;
    REQUIRE(commands.register_command(
        "echo",
        [](const coding_agent::CommandContext&, std::string_view args) {
            return coding_agent::CommandResult{std::string{args}};
        }).has_value());

    coding_agent::prompt::PromptResources resources;
    resources.commands = std::move(commands);
    coding_agent::prompt::PromptProcessor processor{std::move(resources)};

    const auto handled = processor.process("/echo \tkept", {});
    REQUIRE(std::holds_alternative<coding_agent::prompt::CommandHandled>(handled));
    const auto& command = std::get<coding_agent::prompt::CommandHandled>(handled);
    CHECK(command.code == "command_handled");
    CHECK(command.feedback == "\tkept");
    CHECK_FALSE(command.shutdown_requested);

    const auto space_prefixed = processor.process(" /echo ignored", {});
    REQUIRE(std::holds_alternative<coding_agent::prompt::AgentPrompt>(space_prefixed));
    CHECK(std::get<coding_agent::prompt::AgentPrompt>(space_prefixed).text == " /echo ignored");

    const auto tab_prefixed = processor.process("\t/echo ignored", {});
    REQUIRE(std::holds_alternative<coding_agent::prompt::AgentPrompt>(tab_prefixed));
    CHECK(std::get<coding_agent::prompt::AgentPrompt>(tab_prefixed).text == "\t/echo ignored");
}

TEST_CASE("prompt processor applies command then skill then template precedence from its owned snapshot", "[coding_agent][prompt][processor]") {
    auto make_processor = [] {
        coding_agent::prompt::PromptResources resources;
        REQUIRE(resources.commands.register_command(
            "skill:same",
            [](const coding_agent::CommandContext&, std::string_view) {
                return coding_agent::CommandResult{"command won"};
            }).has_value());
        resources.skills.push_back(coding_agent::Skill{
            .name = "same",
            .description = "same-name skill",
            .content = "cached skill body",
            .filePath = "/snapshot/same/SKILL.md",
        });
        resources.templates.push_back(coding_agent::PromptTemplate{
            .name = "skill:same",
            .description = std::nullopt,
            .content = "template won",
        });
        resources.templates.push_back(coding_agent::PromptTemplate{
            .name = "template",
            .description = std::nullopt,
            .content = "template: $1",
        });
        return coding_agent::prompt::PromptProcessor{std::move(resources)};
    };

    auto processor = make_processor();
    const auto command = processor.process("/skill:same ignored", {});
    REQUIRE(std::holds_alternative<coding_agent::prompt::CommandHandled>(command));
    CHECK(std::get<coding_agent::prompt::CommandHandled>(command).feedback == "command won");

    coding_agent::prompt::PromptResources skill_resources;
    skill_resources.skills = processor.skills();
    skill_resources.templates.push_back(coding_agent::PromptTemplate{
        .name = "skill:same",
        .description = std::nullopt,
        .content = "template won",
    });
    coding_agent::prompt::PromptProcessor skill_processor{std::move(skill_resources)};

    const auto skill = skill_processor.process("/skill:same raw instructions", {});
    REQUIRE(std::holds_alternative<coding_agent::prompt::AgentPrompt>(skill));
    const auto& skill_text = std::get<coding_agent::prompt::AgentPrompt>(skill).text;
    CHECK(skill_text.find("<skill name=\"same\"") != std::string::npos);
    CHECK(skill_text.find("cached skill body") != std::string::npos);
    CHECK(skill_text.find("</skill>\n\nraw instructions") != std::string::npos);
    CHECK(skill_text.find("template won") == std::string::npos);
}

TEST_CASE("prompt processor expands templates after command matching without recursive command interpretation", "[coding_agent][prompt][processor]") {
    coding_agent::prompt::PromptResources resources;
    REQUIRE(coding_agent::register_builtin_commands(resources.commands).has_value());
    resources.templates.push_back(coding_agent::PromptTemplate{
        .name = "handoff",
        .description = std::nullopt,
        .content = "/quit $1 ${@:2}",
    });
    coding_agent::prompt::PromptProcessor processor{std::move(resources)};

    const auto expanded = processor.process("/handoff\nfirst second third", {});
    REQUIRE(std::holds_alternative<coding_agent::prompt::AgentPrompt>(expanded));
    CHECK(std::get<coding_agent::prompt::AgentPrompt>(expanded).text == "/quit first second third");

    for (const std::string input : {"/missing", "/", "!echo hi", "!!echo hi"}) {
        const auto passthrough = processor.process(input, {});
        REQUIRE(std::holds_alternative<coding_agent::prompt::AgentPrompt>(passthrough));
        CHECK(std::get<coding_agent::prompt::AgentPrompt>(passthrough).text == input);
    }
}

TEST_CASE("prompt processor contains handler failures and supplies registry-only help metadata", "[coding_agent][prompt][processor]") {
    coding_agent::prompt::PromptResources resources;
    REQUIRE(coding_agent::register_builtin_commands(resources.commands).has_value());
    REQUIRE(resources.commands.register_command(
        "explode",
        [](const coding_agent::CommandContext&, std::string_view) -> coding_agent::CommandResult {
            throw std::runtime_error{"do not expose this"};
        }).has_value());
    resources.skills.push_back(coding_agent::Skill{
        .name = "skill-only",
        .description = "not a registry command",
        .content = "skill",
        .filePath = "/snapshot/skill/SKILL.md",
    });
    resources.templates.push_back(coding_agent::PromptTemplate{
        .name = "template-only",
        .description = std::nullopt,
        .content = "template",
    });
    coding_agent::prompt::PromptProcessor processor{std::move(resources)};

    const auto help = processor.process("/help", {});
    REQUIRE(std::holds_alternative<coding_agent::prompt::CommandHandled>(help));
    const auto& feedback = std::get<coding_agent::prompt::CommandHandled>(help).feedback;
    CHECK(feedback.find("/help") != std::string::npos);
    CHECK(feedback.find("skill-only") == std::string::npos);
    CHECK(feedback.find("template-only") == std::string::npos);

    const auto failed = processor.process("/explode", {});
    REQUIRE(std::holds_alternative<coding_agent::prompt::CommandHandled>(failed));
    const auto& handled = std::get<coding_agent::prompt::CommandHandled>(failed);
    CHECK(handled.code == "command_handler_failed");
    CHECK(handled.feedback == "Command handler failed.");
    CHECK_FALSE(handled.shutdown_requested);
}

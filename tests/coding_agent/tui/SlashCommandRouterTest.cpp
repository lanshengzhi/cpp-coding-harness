#include "coding_agent/tui/SlashCommandRouter.hpp"

#include <cch/support/Error.hpp>
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

using namespace cch;

namespace {

[[nodiscard]] const coding_agent::tui::SlashCommandInvocation* parsed_invocation(
    coding_agent::tui::SlashCommandParseResult& result) {
    return std::get_if<coding_agent::tui::SlashCommandInvocation>(&result);
}

[[nodiscard]] const coding_agent::tui::SlashCommandRouteError* route_error(
    coding_agent::tui::SlashCommandRoute& result) {
    return std::get_if<coding_agent::tui::SlashCommandRouteError>(&result);
}

} // namespace

TEST_CASE(
    "Slash command parsing trims input and resolves aliases",
    "[coding_agent][tui][commands][issue502]") {
    using SlashCommandId = coding_agent::tui::SlashCommandId;
    using enum SlashCommandId;

    struct Case {
        std::string_view text;
        SlashCommandId command;
        std::string_view argument;
    };
    const std::vector<Case> cases{
        {"  /clear  ", Clear, {}},
        {"/new", Clear, {}},
        {"/exit", Quit, {}},
        {"/q", Quit, {}},
        {"/commands", Help, {}},
        {"/scoped-models", Models, {}},
        {"/models", Models, {}},
        {"/model\tprovider/model", Model, "provider/model"},
        {"/login   provider", Login, "provider"},
        {"/name  session name  ", Name, "session name"},
        {"/compact  summarize this  ", Compact, "summarize this"},
    };

    for (const auto& test : cases) {
        auto parsed = coding_agent::tui::SlashCommandRouter::parse(test.text);
        const auto* invocation = parsed_invocation(parsed);
        REQUIRE(invocation != nullptr);
        CHECK(invocation->command == test.command);
        CHECK(invocation->argument == test.argument);
    }

    auto ordinary = coding_agent::tui::SlashCommandRouter::parse("ordinary prompt");
    CHECK(std::holds_alternative<coding_agent::tui::SlashCommandPassThrough>(ordinary));
}

TEST_CASE(
    "Slash command parsing rejects unknown and invalid submissions",
    "[coding_agent][tui][commands][issue502]") {
    auto unknown = coding_agent::tui::SlashCommandRouter::parse("/missing");
    const auto* unknown_error = std::get_if<coding_agent::tui::SlashCommandRouteError>(&unknown);
    REQUIRE(unknown_error != nullptr);
    CHECK(unknown_error->unknown_command);
    CHECK(unknown_error->message == "Unknown slash command '/missing'");

    auto extra_argument = coding_agent::tui::SlashCommandRouter::parse("/copy something");
    const auto* argument_error = std::get_if<coding_agent::tui::SlashCommandRouteError>(
        &extra_argument);
    REQUIRE(argument_error != nullptr);
    CHECK_FALSE(argument_error->unknown_command);
    CHECK(argument_error->message.find("/copy") != std::string::npos);

    auto invalid_thinking = coding_agent::tui::SlashCommandRouter::parse("/thinking turbo");
    const auto* thinking_error = std::get_if<coding_agent::tui::SlashCommandRouteError>(
        &invalid_thinking);
    REQUIRE(thinking_error != nullptr);
    CHECK_FALSE(thinking_error->unknown_command);
    CHECK(thinking_error->message.find("Invalid thinking level") != std::string::npos);

    auto missing_name = coding_agent::tui::SlashCommandRouter::parse("/");
    const auto* missing_name_error = std::get_if<coding_agent::tui::SlashCommandRouteError>(
        &missing_name);
    REQUIRE(missing_name_error != nullptr);
    CHECK_FALSE(missing_name_error->unknown_command);
}

TEST_CASE(
    "Slash command routing executes every immediate command synchronously",
    "[coding_agent][tui][commands][issue502]") {
    using namespace coding_agent::tui;
    using enum SlashCommandId;

    std::vector<SlashCommandId> executed;
    SlashCommandExecutionContext context;
    context.execute_immediate = [&executed](const SlashCommandInvocation& invocation) {
        executed.push_back(invocation.command);
        return support::ExpectedVoid{};
    };
    SlashCommandRouter router;

    const std::vector<std::string_view> commands{
        "/clear", "/quit", "/copy", "/session", "/hotkeys", "/settings", "/help"};
    for (const auto command : commands) {
        auto result = router.route(command, context);
        const auto* immediate = std::get_if<SlashCommandImmediateResult>(&result);
        REQUIRE(immediate != nullptr);
    }

    REQUIRE(executed.size() == commands.size());
    CHECK(executed[0] == Clear);
    CHECK(executed[1] == Quit);
    CHECK(executed[2] == Copy);
    CHECK(executed[3] == Session);
    CHECK(executed[4] == Hotkeys);
    CHECK(executed[5] == Settings);
    CHECK(executed[6] == Help);
}

TEST_CASE(
    "Slash command routing returns structured modal requests with arguments",
    "[coding_agent][tui][commands][issue502]") {
    using namespace coding_agent::tui;
    using enum SlashCommandId;

    SlashCommandExecutionContext context;
    context.execute_immediate = [](const SlashCommandInvocation&) {
        return support::ExpectedVoid{};
    };
    SlashCommandRouter router;

    struct Case {
        std::string_view text;
        SlashCommandId command;
        std::string_view argument;
    };
    const std::vector<Case> cases{
        {"/model fake/model", Model, "fake/model"},
        {"/models", Models, {}},
        {"/thinking high", Thinking, "high"},
        {"/login fake", Login, "fake"},
        {"/logout", Logout, {}},
        {"/resume", Resume, {}},
        {"/fork", Fork, {}},
        {"/tree", Tree, {}},
        {"/reload", Reload, {}},
        {"/compact summarize", Compact, "summarize"},
    };

    for (const auto& test : cases) {
        auto result = router.route(test.text, context);
        const auto* modal = std::get_if<SlashCommandModalResult>(&result);
        REQUIRE(modal != nullptr);
        CHECK(modal->invocation.command == test.command);
        CHECK(modal->invocation.argument == test.argument);
    }
}

TEST_CASE(
    "Slash command routing preserves registered dynamic slash resources",
    "[coding_agent][tui][commands][issue502]") {
    using namespace coding_agent::tui;

    SlashCommandExecutionContext context;
    context.allow_unrecognized = [](std::string_view command) {
        return command == "project-prompt" || command == "skill:review";
    };
    SlashCommandRouter router;

    auto prompt = router.route("  /project-prompt details  ", context);
    CHECK(std::holds_alternative<SlashCommandPassThrough>(prompt));

    auto skill = router.route("/skill:review", context);
    CHECK(std::holds_alternative<SlashCommandPassThrough>(skill));

    auto unknown = router.route("/missing", context);
    const auto* error = route_error(unknown);
    REQUIRE(error != nullptr);
    CHECK(error->unknown_command);
}

TEST_CASE(
    "Slash command routing reports immediate execution failures as user-visible errors",
    "[coding_agent][tui][commands][issue502]") {
    using namespace coding_agent::tui;

    SlashCommandExecutionContext context;
    context.execute_immediate = [](const SlashCommandInvocation&) {
        return support::ExpectedVoid{std::unexpected(support::make_error(
            support::ErrorCode::Process,
            "clipboard unavailable"))};
    };
    SlashCommandRouter router;

    auto result = router.route("/copy", context);
    const auto* error = route_error(result);
    REQUIRE(error != nullptr);
    CHECK(error->message == "Could not execute /copy: clipboard unavailable");
}

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

namespace tui = coding_agent::tui;
using SlashCommandId = tui::SlashCommandId;
using SlashCommandRouteErrorKind = tui::SlashCommandRouteErrorKind;

[[nodiscard]] const tui::SlashCommandInvocation* parsed_invocation(
    tui::SlashCommandParseResultVariant& result) {
    return std::get_if<tui::SlashCommandInvocation>(&result);
}

[[nodiscard]] const tui::SlashCommandRouteError* route_error(
    tui::SlashCommandRouteVariant& result) {
    return std::get_if<tui::SlashCommandRouteError>(&result);
}

} // namespace

TEST_CASE(
    "Slash command parsing trims input and resolves aliases",
    "[coding_agent][tui][commands][issue502]") {
    using enum SlashCommandId;

    struct Case {
        std::string_view text;
        SlashCommandId command;
        std::string_view argument;
    };
    const std::vector<Case> cases{
        {.text = "  /clear  ", .command = Clear, .argument = {}},
        {.text = "/new", .command = Clear, .argument = {}},
        {.text = "/exit", .command = Quit, .argument = {}},
        {.text = "/q", .command = Quit, .argument = {}},
        {.text = "/commands", .command = Help, .argument = {}},
        {.text = "/scoped-models", .command = Models, .argument = {}},
        {.text = "/models", .command = Models, .argument = {}},
        {.text = "/model\tprovider/model", .command = Model, .argument = "provider/model"},
        {.text = "/login   provider", .command = Login, .argument = "provider"},
        {.text = "/name  session name  ", .command = Name, .argument = "session name"},
        {.text = "/compact  summarize this  ",
         .command = Compact,
         .argument = "summarize this"},
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
    CHECK(unknown_error->kind == SlashCommandRouteErrorKind::UnknownCommand);
    CHECK(unknown_error->message == "Unknown slash command '/missing'");

    auto extra_argument = coding_agent::tui::SlashCommandRouter::parse("/copy something");
    const auto* argument_error = std::get_if<coding_agent::tui::SlashCommandRouteError>(
        &extra_argument);
    REQUIRE(argument_error != nullptr);
    CHECK(argument_error->kind == SlashCommandRouteErrorKind::Invalid);
    CHECK(argument_error->message.find("/copy") != std::string::npos);

    auto invalid_thinking = coding_agent::tui::SlashCommandRouter::parse("/thinking turbo");
    const auto* thinking_error = std::get_if<coding_agent::tui::SlashCommandRouteError>(
        &invalid_thinking);
    REQUIRE(thinking_error != nullptr);
    CHECK(thinking_error->kind == SlashCommandRouteErrorKind::Invalid);
    CHECK(thinking_error->message.find("Invalid thinking level") != std::string::npos);

    auto missing_name = coding_agent::tui::SlashCommandRouter::parse("/");
    const auto* missing_name_error = std::get_if<coding_agent::tui::SlashCommandRouteError>(
        &missing_name);
    REQUIRE(missing_name_error != nullptr);
    CHECK(missing_name_error->kind == SlashCommandRouteErrorKind::Invalid);
}

TEST_CASE(
    "Slash command routing executes every immediate command synchronously",
    "[coding_agent][tui][commands][issue502]") {
    using enum SlashCommandId;

    std::vector<SlashCommandId> executed;
    tui::SlashCommandExecutionContext context;
    context.execute_immediate = [&executed](const tui::SlashCommandInvocation& invocation) {
        executed.push_back(invocation.command);
        return support::ExpectedVoid{};
    };
    tui::SlashCommandRouter router;

    const std::vector<std::string_view> commands{
        "/clear", "/quit", "/copy", "/session", "/hotkeys", "/settings", "/help",
        "/name renamed"};
    for (const auto& command : commands) {
        auto result = router.route(command, context);
        const auto* immediate = std::get_if<tui::SlashCommandImmediateResult>(&result);
        CHECK(immediate != nullptr);
    }

    REQUIRE(executed.size() == commands.size());
    CHECK(executed[0] == Clear);
    CHECK(executed[1] == Quit);
    CHECK(executed[2] == Copy);
    CHECK(executed[3] == Session);
    CHECK(executed[4] == Hotkeys);
    CHECK(executed[5] == Settings);
    CHECK(executed[6] == Help);
    CHECK(executed[7] == Name);
}

TEST_CASE(
    "Slash command routing returns structured modal requests with arguments",
    "[coding_agent][tui][commands][issue502]") {
    using enum SlashCommandId;

    tui::SlashCommandExecutionContext context;
    context.execute_immediate = [](const tui::SlashCommandInvocation&) {
        return support::ExpectedVoid{};
    };
    tui::SlashCommandRouter router;

    struct Case {
        std::string_view text;
        SlashCommandId command;
        std::string_view argument;
    };
    const std::vector<Case> cases{
        {.text = "/model fake/model", .command = Model, .argument = "fake/model"},
        {.text = "/models", .command = Models, .argument = {}},
        {.text = "/thinking high", .command = Thinking, .argument = "high"},
        {.text = "/login fake", .command = Login, .argument = "fake"},
        {.text = "/logout", .command = Logout, .argument = {}},
        {.text = "/resume", .command = Resume, .argument = {}},
        {.text = "/fork", .command = Fork, .argument = {}},
        {.text = "/tree", .command = Tree, .argument = {}},
        {.text = "/reload", .command = Reload, .argument = {}},
        {.text = "/compact summarize", .command = Compact, .argument = "summarize"},
        {.text = "/trust", .command = Trust, .argument = {}},
    };

    for (const auto& test : cases) {
        auto result = router.route(test.text, context);
        const auto* modal = std::get_if<tui::SlashCommandModalResult>(&result);
        REQUIRE(modal != nullptr);
        CHECK(modal->invocation.command == test.command);
        CHECK(modal->invocation.argument == test.argument);
    }
}

TEST_CASE(
    "Slash command routing preserves host-recognized resources and paths",
    "[coding_agent][tui][commands][issue502]") {
    tui::SlashCommandExecutionContext context;
    context.allow_unrecognized = [](std::string_view command) {
        return command == "project-prompt" || command == "skill:review" ||
            command == "tmp/clipboard.png";
    };
    tui::SlashCommandRouter router;

    auto prompt = router.route("  /project-prompt details  ", context);
    CHECK(std::holds_alternative<tui::SlashCommandPassThrough>(prompt));

    auto skill = router.route("/skill:review", context);
    CHECK(std::holds_alternative<tui::SlashCommandPassThrough>(skill));

    auto clipboard_path = router.route("/tmp/clipboard.png", context);
    CHECK(std::holds_alternative<tui::SlashCommandPassThrough>(clipboard_path));

    auto unknown = router.route("/missing", context);
    const auto* error = route_error(unknown);
    REQUIRE(error != nullptr);
    CHECK(error->kind == SlashCommandRouteErrorKind::UnknownCommand);
}

TEST_CASE(
    "Slash command routing reports immediate execution failures as user-visible errors",
    "[coding_agent][tui][commands][issue502]") {
    tui::SlashCommandExecutionContext context;
    context.execute_immediate = [](const tui::SlashCommandInvocation&) {
        return support::ExpectedVoid{std::unexpected(support::make_error(
            support::ErrorCode::Process,
            "clipboard unavailable"))};
    };
    tui::SlashCommandRouter router;

    auto result = router.route("/copy", context);
    const auto* error = route_error(result);
    REQUIRE(error != nullptr);
    CHECK(error->message == "Could not execute /copy: clipboard unavailable");
}

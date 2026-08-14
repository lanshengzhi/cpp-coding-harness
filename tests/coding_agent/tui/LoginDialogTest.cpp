// pi `login-dialog.ts`: the login dialog that replaces the editor during the
// OAuth and API-key login flows (G2 decision 4). Component-level coverage of
// the auth URL / device code / manual code / prompt / info / waiting /
// progress views, input submission, and the two cancellation paths (dialog
// stop source and per-prompt race) through the public component surface.

#include "coding_agent/tui/LoginDialog.hpp"
#include "coding_agent/tui/Theme.hpp"

#include <cch/tui/Keybindings.hpp>

#include <cch/util/Error.hpp>
#include <catch2/catch_test_macros.hpp>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace cch;

namespace {

[[nodiscard]] std::shared_ptr<const tui::KeybindingRegistry> test_keybindings() {
    tui::KeybindingResolutionRequest request;
    request.definitions = tui::builtin_tui_keybinding_definitions();
    auto resolved = tui::resolve_keybindings(std::move(request));
    REQUIRE(resolved);
    return resolved->registry;
}

[[nodiscard]] coding_agent::tui::LiveTheme test_theme() {
    return coding_agent::tui::LiveTheme(
        coding_agent::tui::builtin_dark_theme(),
        tui::TerminalColorCapability::TrueColor);
}

void drain_ready(boost::asio::io_context& io) {
    if (io.stopped()) io.restart();
    while (io.poll() != 0) {
    }
}

/// Remove SGR (`ESC [ … final`) sequences so assertions target the visible
/// text; OSC hyperlinks keep their display text.
[[nodiscard]] std::string strip_ansi(std::string_view text) {
    std::string stripped;
    stripped.reserve(text.size());
    for (std::size_t index = 0; index < text.size();) {
        if (text[index] == '\x1b' && index + 1 < text.size() && text[index + 1] == '[') {
            index += 2;
            while (index < text.size() &&
                   !(text[index] >= '@' && text[index] <= '~')) {
                ++index;
            }
            if (index < text.size()) ++index;
            continue;
        }
        if (text[index] == '\x1b' && index + 1 < text.size() && text[index + 1] == ']') {
            index += 2;
            while (index < text.size() && text[index] != '\a') ++index;
            if (index < text.size()) ++index;
            continue;
        }
        stripped.push_back(text[index]);
        ++index;
    }
    return stripped;
}

[[nodiscard]] std::string screen_of(cch::tui::Component& component, std::size_t width = 80) {
    const auto rendered = component.render(width);
    REQUIRE(rendered);
    std::string text;
    for (const auto& line : rendered->lines) {
        text.append(strip_ansi(line));
        text.push_back('\n');
    }
    return text;
}

struct DialogFixture {
    coding_agent::tui::LiveTheme theme{test_theme()};
    std::vector<std::string> opened_urls;
    std::size_t invalidations = 0;
    std::size_t cancellations = 0;

    [[nodiscard]] coding_agent::tui::LoginDialogComponent make(std::string title) {
        return coding_agent::tui::LoginDialogComponent(
            theme,
            test_keybindings(),
            std::move(title),
            [this] { ++invalidations; },
            [this](std::string url) { opened_urls.push_back(std::move(url)); },
            [this] { ++cancellations; });
    }
};

void type(cch::tui::InputHandler& handler, std::string text) {
    for (const char ch : text) {
        handler.handle_input(tui::KeyEvent{.key = std::string(1, ch)});
    }
}

} // namespace

TEST_CASE(
    "LoginDialog renders the auth URL view and opens the browser",
    "[coding_agent][tui][login][issue406]") {
    DialogFixture fixture;
    auto dialog = fixture.make("Login to OpenAI Codex");

    dialog.show_auth(
        "https://auth.openai.example/authorize?client=abc",
        "Authorize access to your OpenAI account.");

    const auto screen = screen_of(dialog);
    CHECK(screen.find("Login to OpenAI Codex") != std::string::npos);
    CHECK(screen.find("https://auth.openai.example/authorize?client=abc") != std::string::npos);
    CHECK(screen.find("click to open") != std::string::npos);
    CHECK(screen.find("Authorize access to your OpenAI account.") != std::string::npos);
    // pi opens the browser best-effort with the presented URL.
    REQUIRE(fixture.opened_urls.size() == 1);
    CHECK(fixture.opened_urls[0] == "https://auth.openai.example/authorize?client=abc");
    CHECK(fixture.invalidations > 0);
}

TEST_CASE(
    "LoginDialog renders the device code view with the waiting hint",
    "[coding_agent][tui][login][issue406]") {
    DialogFixture fixture;
    auto dialog = fixture.make("Login to Kimi For Coding");

    dialog.show_device_code("ABCD-EFGH", "https://kimi.example/device");
    dialog.show_waiting("Waiting for authentication...");

    const auto screen = screen_of(dialog);
    CHECK(screen.find("https://kimi.example/device") != std::string::npos);
    CHECK(screen.find("Enter code: ABCD-EFGH") != std::string::npos);
    CHECK(screen.find("Waiting for authentication...") != std::string::npos);
    CHECK(screen.find("to cancel") != std::string::npos);
}

TEST_CASE(
    "LoginDialog renders info lines with links and the close hint",
    "[coding_agent][tui][login][issue406]") {
    DialogFixture fixture;
    auto dialog = fixture.make("Kimi For Coding setup");

    dialog.show_info(
        "Kimi API key is configured outside cch.",
        {{"https://kimi.example/docs", "Docs"}},
        true);

    const auto screen = screen_of(dialog);
    CHECK(screen.find("Kimi API key is configured outside cch.") != std::string::npos);
    CHECK(screen.find("Docs: https://kimi.example/docs") != std::string::npos);
    CHECK(screen.find("to close") != std::string::npos);
    // The info branch never opens a browser.
    CHECK(fixture.opened_urls.empty());
}

TEST_CASE(
    "LoginDialog prompt submits the entered value and freezes it as echoed text",
    "[coding_agent][tui][login][issue406]") {
    DialogFixture fixture;
    auto dialog = fixture.make("Login to DeepSeek");

    dialog.show_progress("Contacting provider...");

    boost::asio::io_context io;
    std::optional<util::Expected<std::string>> prompt_result;
    boost::asio::co_spawn(
        io,
        dialog.show_prompt("Enter API key", "sk-..."),
        [&](std::exception_ptr exception, util::Expected<std::string> result) {
            CHECK(exception == nullptr);
            prompt_result.emplace(std::move(result));
        });
    drain_ready(io);

    {
        const auto screen = screen_of(dialog);
        CHECK(screen.find("Contacting provider...") != std::string::npos);
        CHECK(screen.find("Enter API key") != std::string::npos);
        CHECK(screen.find("e.g., sk-...") != std::string::npos);
        CHECK(screen.find("to cancel,") != std::string::npos);
        CHECK(screen.find("to submit") != std::string::npos);
    }

    type(dialog, "sk-dummy");
    dialog.handle_input(tui::KeyEvent{.key = "enter"});
    drain_ready(io);

    REQUIRE(prompt_result.has_value());
    REQUIRE(prompt_result->has_value());
    CHECK(**prompt_result == "sk-dummy");

    // pi replaceInputWithSubmittedText: the input becomes static echoed text.
    const auto screen = screen_of(dialog);
    CHECK(screen.find("> sk-dummy") != std::string::npos);
    CHECK(fixture.cancellations == 0);
    CHECK_FALSE(dialog.stop_token().stop_requested());
}

TEST_CASE(
    "LoginDialog escape cancels the pending prompt and fires the stop source",
    "[coding_agent][tui][login][issue406]") {
    DialogFixture fixture;
    auto dialog = fixture.make("Login to OpenAI Codex");

    boost::asio::io_context io;
    std::optional<util::Expected<std::string>> prompt_result;
    boost::asio::co_spawn(
        io,
        dialog.show_manual_input("Paste the authorization code"),
        [&](std::exception_ptr exception, util::Expected<std::string> result) {
            CHECK(exception == nullptr);
            prompt_result.emplace(std::move(result));
        });
    drain_ready(io);
    CHECK(screen_of(dialog).find("Paste the authorization code") != std::string::npos);

    dialog.handle_input(tui::KeyEvent{.key = "escape"});
    drain_ready(io);

    REQUIRE(prompt_result.has_value());
    REQUIRE_FALSE(prompt_result->has_value());
    CHECK(prompt_result->error().code == util::ErrorCode::Cancelled);
    CHECK(prompt_result->error().message == "Login cancelled");
    CHECK(dialog.stop_token().stop_requested());
    CHECK(fixture.cancellations == 1);
}

TEST_CASE(
    "LoginDialog per-prompt cancellation rejects without firing the stop source",
    "[coding_agent][tui][login][issue406]") {
    DialogFixture fixture;
    auto dialog = fixture.make("Login to OpenAI Codex");

    boost::asio::io_context io;
    std::optional<util::Expected<std::string>> prompt_result;
    boost::asio::co_spawn(
        io,
        dialog.show_manual_input("Paste the authorization code"),
        [&](std::exception_ptr exception, util::Expected<std::string> result) {
            CHECK(exception == nullptr);
            prompt_result.emplace(std::move(result));
        });
    drain_ready(io);

    // The Codex callback-vs-manual-code race: the callback winning cancels
    // the pending prompt only (pi `manualAbort`) — the flow itself continues.
    dialog.cancel_pending_prompt();
    drain_ready(io);

    REQUIRE(prompt_result.has_value());
    REQUIRE_FALSE(prompt_result->has_value());
    CHECK(prompt_result->error().code == util::ErrorCode::Cancelled);
    CHECK(prompt_result->error().message == "Login cancelled");
    CHECK_FALSE(dialog.stop_token().stop_requested());
    CHECK(fixture.cancellations == 0);
}

TEST_CASE(
    "LoginDialog serves sequential prompts, echoing each submitted value",
    "[coding_agent][tui][login][issue406]") {
    DialogFixture fixture;
    auto dialog = fixture.make("Login to DeepSeek");
    boost::asio::io_context io;

    std::optional<util::Expected<std::string>> first_result;
    boost::asio::co_spawn(
        io,
        dialog.show_prompt("Enter API key", std::nullopt),
        [&](std::exception_ptr exception, util::Expected<std::string> result) {
            CHECK(exception == nullptr);
            first_result.emplace(std::move(result));
        });
    drain_ready(io);
    type(dialog, "first-value");
    dialog.handle_input(tui::KeyEvent{.key = "enter"});
    drain_ready(io);
    REQUIRE(first_result.has_value());
    REQUIRE(first_result->has_value());
    CHECK(**first_result == "first-value");

    std::optional<util::Expected<std::string>> second_result;
    boost::asio::co_spawn(
        io,
        dialog.show_prompt("Confirm API key", std::nullopt),
        [&](std::exception_ptr exception, util::Expected<std::string> result) {
            CHECK(exception == nullptr);
            second_result.emplace(std::move(result));
        });
    drain_ready(io);
    type(dialog, "second-value");
    dialog.handle_input(tui::KeyEvent{.key = "enter"});
    drain_ready(io);
    REQUIRE(second_result.has_value());
    REQUIRE(second_result->has_value());
    CHECK(**second_result == "second-value");

    const auto screen = screen_of(dialog);
    CHECK(screen.find("> first-value") != std::string::npos);
    CHECK(screen.find("> second-value") != std::string::npos);
    CHECK(screen.find("Confirm API key") != std::string::npos);
}

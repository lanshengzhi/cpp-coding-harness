// pi `extension-selector.ts` (the generic string-list selector — the C++
// name drops the misleading "extension" label, G2 decision 4) and pi
// `dynamic-border.ts`: the login auth-type picker and `select`-type
// AuthPrompt surface. Component-level coverage of rendering, navigation,
// selection, and cancellation through the public component surface.

#include "coding_agent/tui/DynamicBorder.hpp"
#include "coding_agent/tui/StringListSelector.hpp"
#include "coding_agent/tui/Theme.hpp"

#include <cch/tui/Keybindings.hpp>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace cch;

namespace {

[[nodiscard]] std::shared_ptr<const tui::KeybindingRegistry> test_keybindings() {
    tui::KeybindingResolutionRequest request;
    request.definitions = tui::builtin_tui_keybinding_definitions();
    request.definitions.push_back({
        .id = "app.tools.expand",
        .default_keys = {"ctrl+o"},
        .description = "Expand tool output",
        .category = "App",
    });
    auto resolved = tui::resolve_keybindings(std::move(request));
    REQUIRE(resolved);
    return resolved->registry;
}

[[nodiscard]] coding_agent::tui::LiveTheme test_theme() {
    return coding_agent::tui::LiveTheme(
        coding_agent::tui::builtin_dark_theme(),
        tui::TerminalColorCapability::TrueColor);
}

/// Remove SGR (`ESC [ … final`) and OSC hyperlink (`ESC ] … BEL`) sequences
/// so assertions target the visible text.
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

[[nodiscard]] std::string join_lines(const std::vector<std::string>& lines) {
    std::string text;
    for (const auto& line : lines) {
        text.append(strip_ansi(line));
        text.push_back('\n');
    }
    return text;
}

} // namespace

TEST_CASE(
    "DynamicBorder renders one width-filled line through its color hook",
    "[coding_agent][tui][login][issue406]") {
    coding_agent::tui::DynamicBorder border(
        [](std::string text) { return "<" + text + ">"; });
    const auto rendered = border.render(12);
    REQUIRE(rendered);
    REQUIRE(rendered->lines.size() == 1);
    CHECK(rendered->lines[0] == "<────────────>");
}

TEST_CASE(
    "StringListSelector renders title, options, and hints with the first option selected",
    "[coding_agent][tui][login][issue406]") {
    auto theme = test_theme();
    std::optional<std::string> selected;
    std::size_t cancellations = 0;
    coding_agent::tui::StringListSelector selector(
        theme,
        test_keybindings(),
        "Select authentication method:",
        {"Sign in with an account", "Sign in with an API key"},
        [&selected](std::string option) { selected = std::move(option); },
        [&cancellations] { ++cancellations; });

    const auto rendered = selector.render(60);
    REQUIRE(rendered);
    const auto screen = join_lines(rendered->lines);
    CHECK(screen.find("Select authentication method:") != std::string::npos);
    CHECK(screen.find("→ Sign in with an account") != std::string::npos);
    CHECK(screen.find("  Sign in with an API key") != std::string::npos);
    CHECK(screen.find("navigate") != std::string::npos);
    CHECK(screen.find("select") != std::string::npos);
    CHECK(screen.find("cancel") != std::string::npos);
    CHECK_FALSE(selected.has_value());
    CHECK(cancellations == 0);
}

TEST_CASE(
    "StringListSelector navigates with arrows and j/k, clamped at the edges",
    "[coding_agent][tui][login][issue406]") {
    auto theme = test_theme();
    std::optional<std::string> selected;
    coding_agent::tui::StringListSelector selector(
        theme,
        test_keybindings(),
        "Pick:",
        {"one", "two", "three"},
        [&selected](std::string option) { selected = std::move(option); },
        [] {});

    selector.handle_input(tui::KeyEvent{.key = "down"});
    {
        const auto rendered = selector.render(40);
        REQUIRE(rendered);
        CHECK(join_lines(rendered->lines).find("→ two") != std::string::npos);
    }
    selector.handle_input(tui::KeyEvent{.key = "j"});
    {
        const auto rendered = selector.render(40);
        REQUIRE(rendered);
        CHECK(join_lines(rendered->lines).find("→ three") != std::string::npos);
    }
    // Clamped at the bottom: no wraparound (pi Math.min).
    selector.handle_input(tui::KeyEvent{.key = "down"});
    selector.handle_input(tui::KeyEvent{.key = "enter"});
    CHECK(selected == "three");

    selector.handle_input(tui::KeyEvent{.key = "k"});
    selector.handle_input(tui::KeyEvent{.key = "up"});
    // Clamped at the top (pi Math.max).
    selector.handle_input(tui::KeyEvent{.key = "up"});
    selector.handle_input(tui::KeyEvent{.key = "enter"});
    CHECK(selected == "one");
}

TEST_CASE(
    "StringListSelector cancels on escape and ctrl+c",
    "[coding_agent][tui][login][issue406]") {
    auto theme = test_theme();
    std::size_t cancellations = 0;
    coding_agent::tui::StringListSelector selector(
        theme,
        test_keybindings(),
        "Pick:",
        {"one"},
        [](std::string) {},
        [&cancellations] { ++cancellations; });

    selector.handle_input(tui::KeyEvent{.key = "escape"});
    CHECK(cancellations == 1);
    selector.handle_input(tui::KeyEvent{.key = "c", .ctrl = true});
    CHECK(cancellations == 2);
}

TEST_CASE(
    "StringListSelector routes app.tools.expand to its toggle sink",
    "[coding_agent][tui][login][issue406]") {
    auto theme = test_theme();
    std::size_t toggles = 0;
    coding_agent::tui::StringListSelector selector(
        theme,
        test_keybindings(),
        "Pick:",
        {"one"},
        [](std::string) {},
        [] {},
        coding_agent::tui::StringListSelectorOptions{
            .on_toggle_tools_expanded = [&toggles] { ++toggles; },
        });

    selector.handle_input(tui::KeyEvent{.key = "o", .ctrl = true});
    CHECK(toggles == 1);
}

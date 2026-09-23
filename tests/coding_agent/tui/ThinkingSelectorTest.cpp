// The thinking-level selector (pi `thinking-selector.ts`): renders the
// available levels with pi's per-level descriptions, the current-level
// marker, the ` · default` marker on the settings default level, and the
// save-as-default affordance; Enter selects through the callback, Ctrl+S
// (`app.thinking.save`) saves the default, and Escape cancels.

#include "coding_agent/tui/ThinkingSelector.hpp"
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
    // The thinking selector's cycle and save actions are application-level
    // definitions (the host registers them through the keybindings manager);
    // mirror the production binding set.
    request.definitions.push_back({
            .id = "app.thinking.cycle",
            .default_keys = {"shift+tab"},
            .description = {},
            .category = {},
    });
    request.definitions.push_back({
            .id = "app.thinking.save",
            .default_keys = {"ctrl+s"},
            .description = {},
            .category = {},
    });
    auto resolved = tui::resolve_keybindings(std::move(request));
    REQUIRE(resolved);
    return resolved->registry;
}

[[nodiscard]] coding_agent::tui::LiveTheme test_theme() {
    return coding_agent::tui::LiveTheme(
            coding_agent::tui::builtin_dark_theme(), tui::TerminalColorCapability::TrueColor);
}

[[nodiscard]] std::string strip_ansi(std::string_view text) {
    std::string stripped;
    stripped.reserve(text.size());
    for (std::size_t index = 0; index < text.size();) {
        if (text[index] == '\x1b' && index + 1 < text.size() && text[index + 1] == '[') {
            index += 2;
            while (index < text.size() && !(text[index] >= '@' && text[index] <= '~'))
                ++index;
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

TEST_CASE("ThinkingSelector renders the title, level descriptions, and the default marker",
        "[coding_agent][tui][thinking-selector][issue774][spec]") {
    auto theme = test_theme();
    std::optional<std::string> selected;
    std::optional<std::string> saved;
    std::size_t cancellations = 0;
    auto selector = std::make_shared<coding_agent::tui::ThinkingSelectorComponent>(
            theme,
            test_keybindings(),
            "low",
            std::vector<std::string>{"off", "minimal", "low", "medium", "high"},
            [&selected](std::string level) { selected = std::move(level); },
            [&cancellations] { ++cancellations; },
            [&saved](std::string level) { saved = std::move(level); },
            std::optional<std::string>{"low"});

    const auto rendered = selector->render(120);
    REQUIRE(rendered);
    const auto screen = join_lines(rendered->lines);
    CHECK(screen.find("Thinking Level") != std::string::npos);
    // pi `LEVEL_DESCRIPTIONS` plus the ` · default` marker on the default row.
    CHECK(screen.find("No reasoning") != std::string::npos);
    CHECK(screen.find("Deep reasoning (~16k tokens)") != std::string::npos);
    CHECK(screen.find("Light reasoning (~2k tokens) · default") != std::string::npos);
    // pi's current marker and the cycle hint line.
    CHECK(screen.find("✓ low") != std::string::npos);
    CHECK(screen.find("Shift+Tab cycles thinking levels in-session") != std::string::npos);
    // pi's constructor-gated save hint.
    CHECK(screen.find("Ctrl+S to set as default") != std::string::npos);
    CHECK_FALSE(selected.has_value());
    CHECK_FALSE(saved.has_value());
    CHECK(cancellations == 0);
}

TEST_CASE("ThinkingSelector selects on Enter, saves the default on Ctrl+S, and cancels on Escape",
        "[coding_agent][tui][thinking-selector][issue774][spec]") {
    auto theme = test_theme();
    std::optional<std::string> selected;
    std::optional<std::string> saved;
    std::size_t cancellations = 0;
    auto selector = std::make_shared<coding_agent::tui::ThinkingSelectorComponent>(
            theme,
            test_keybindings(),
            "medium",
            std::vector<std::string>{"off", "minimal", "low", "medium", "high"},
            [&selected](std::string level) { selected = std::move(level); },
            [&cancellations] { ++cancellations; },
            [&saved](std::string level) { saved = std::move(level); },
            std::nullopt);

    // Enter selects the highlighted level (pi `onSelect`).
    static_cast<void>(selector->handle_input(tui::KeyEvent{.key = "enter"}));
    REQUIRE(selected.has_value());
    CHECK(*selected == "medium");
    CHECK_FALSE(saved.has_value());

    // Ctrl+S (`app.thinking.save`) fires the save-as-default sink with the
    // highlighted level (pi `onSelectAsDefault`).
    static_cast<void>(selector->handle_input(tui::KeyEvent{.key = "s", .ctrl = true}));
    REQUIRE(saved.has_value());
    CHECK(*saved == "medium");

    // Escape cancels through the SelectList (pi `tui.select.cancel`).
    static_cast<void>(selector->handle_input(tui::KeyEvent{.key = "escape"}));
    CHECK(cancellations == 1);
}

TEST_CASE("ThinkingSelector omits the save hint without a save-as-default sink",
        "[coding_agent][tui][thinking-selector][issue774][spec]") {
    auto theme = test_theme();
    std::optional<std::string> selected;
    auto selector = std::make_shared<coding_agent::tui::ThinkingSelectorComponent>(
            theme,
            test_keybindings(),
            "medium",
            std::vector<std::string>{"off", "medium", "high"},
            [&selected](std::string level) { selected = std::move(level); },
            [] {},
            coding_agent::tui::ThinkingSelectorSelectAsDefaultSink{},
            std::nullopt);

    const auto rendered = selector->render(70);
    REQUIRE(rendered);
    const auto screen = join_lines(rendered->lines);
    CHECK(screen.find("to set as default") == std::string::npos);

    // Without the sink the save key falls through to the search input and
    // never fires a selection.
    static_cast<void>(selector->handle_input(tui::KeyEvent{.key = "s", .ctrl = true}));
    static_cast<void>(selector->handle_input(tui::KeyEvent{.key = "enter"}));
    REQUIRE(selected.has_value());
    CHECK(*selected == "medium");
}

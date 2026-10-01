// The thinking-level selector (pi `thinking-selector.ts`): renders the
// available levels with pi's per-level descriptions, the current-level
// marker, the ` · default` marker on the settings default level, and the
// save-as-default affordance; Enter selects through the callback, Ctrl+S
// (`app.thinking.save`) saves the default, and Escape cancels.

#include "coding_agent/tui/ThinkingSelector.hpp"
#include "coding_agent/tui/Theme.hpp"

#include <cch/tui/Keybindings.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
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

/// One rendered row with its styling removed and its trailing padding
/// dropped, so a row compares by its content (pi's `Spacer` rows are empty
/// and `Text` rows are padded to the render width).
[[nodiscard]] std::string row_text(const std::string& line) {
    std::string text = strip_ansi(line);
    while (!text.empty() && text.back() == ' ')
        text.pop_back();
    return text;
}

[[nodiscard]] std::vector<std::string> row_texts(const std::vector<std::string>& lines) {
    std::vector<std::string> texts;
    texts.reserve(lines.size());
    for (const auto& line : lines)
        texts.push_back(row_text(line));
    return texts;
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

TEST_CASE("ThinkingSelector renders pi's blank rows between the selector chrome blocks",
        "[coding_agent][tui][thinking-selector][issue810][spec]") {
    auto theme = test_theme();
    std::optional<std::string> selected;
    auto selector = std::make_shared<coding_agent::tui::ThinkingSelectorComponent>(
            theme,
            test_keybindings(),
            "off",
            std::vector<std::string>{"off", "minimal", "low", "medium", "high"},
            [&selected](std::string level) { selected = std::move(level); },
            [] {},
            [](std::string) {},
            std::nullopt);

    const auto rendered = selector->render(100);
    REQUIRE(rendered);
    const auto rows = row_texts(rendered->lines);
    const auto row_of = [&rows](std::string_view needle) {
        return std::ranges::find_if(
                rows, [needle](const std::string& row) { return row.find(needle) != std::string::npos; });
    };

    const auto top_border = row_of("──");
    const auto title = row_of("Thinking Level");
    const auto cycle_hint = row_of("cycles thinking levels in-session");
    const auto search = std::ranges::find(rows, ">");
    const auto command_row = row_of("to set as default");
    const auto index_of = [&rows](const std::vector<std::string>::const_iterator found) {
        return static_cast<std::size_t>(found - rows.begin());
    };
    REQUIRE(top_border != rows.end());
    REQUIRE(title != rows.end());
    REQUIRE(cycle_hint != rows.end());
    REQUIRE(command_row != rows.end());
    REQUIRE(search != rows.end());
    const auto border_row = index_of(top_border);
    const auto command_index = index_of(command_row);
    REQUIRE(command_index + 1 < rows.size());
    // pi's `Spacer(1)` between every chrome block. `cch::tui::Text` with
    // empty text renders no row at all, so a Text-based spacer silently
    // drops all four of them and the block closes up.
    CHECK(rows[border_row + 1].empty());
    CHECK(rows[index_of(title) + 1].empty());
    CHECK(rows[index_of(cycle_hint) + 1].empty());
    CHECK(rows[command_index - 1].empty());
    // pi brackets the block with its dynamic border and draws the command
    // affordance as the last row inside it.
    CHECK(rows[border_row + 2] == "Thinking Level");
    CHECK(rows[command_index + 1] == *top_border);
}

TEST_CASE("ThinkingSelector renders pi's level rows at pi's thinking select list label column",
        "[coding_agent][tui][thinking-selector][issue810][spec]") {
    auto theme = test_theme();
    const auto selector_with = [&theme](std::vector<std::string> levels, std::string current) {
        return std::make_shared<coding_agent::tui::ThinkingSelectorComponent>(
                theme,
                test_keybindings(),
                std::move(current),
                std::move(levels),
                [](std::string) {},
                [] {},
                [](std::string) {},
                std::nullopt);
    };

    // pi `THINKING_SELECT_LIST_LAYOUT` clamps the label column into [12, 32]
    // and pi draws the description after that column plus the two-column
    // selection prefix. These rows are the captured pi rows for the levels
    // both runtimes offer (`report.json:thinking-selector`, pi snapshot 2);
    // the toolkit default column (32) would place every description eight
    // columns further right.
    const std::vector<std::string> shared_rows = {
            "→ ✓ off       No reasoning",
            "    minimal   Very brief reasoning (~1k tokens)",
            "    low       Light reasoning (~2k tokens)",
            "    medium    Moderate reasoning (~8k tokens)",
            "    high      Deep reasoning (~16k tokens)",
    };
    const auto shared = selector_with({"off", "minimal", "low", "medium", "high"}, "off");
    const auto shared_rendered = shared->render(100);
    REQUIRE(shared_rendered);
    const auto shared_texts = row_texts(shared_rendered->lines);
    for (const auto& expected : shared_rows) {
        INFO("row " << expected);
        CHECK(std::ranges::find(shared_texts, expected) != shared_texts.end());
    }

    // The separating case: the wider level set the model metadata on this
    // side offers keeps the same column for the longer names, because the
    // column is clamped at pi's 12-column minimum rather than tracking the
    // widest label. A column derived from the labels present (with or
    // without the maximum) would move these two rows, and the rows above
    // pass either way, so both cases are needed to separate the layout
    // options from a hardcoded spacing.
    const auto extended = selector_with({"off", "minimal", "low", "medium", "high", "xhigh", "max"}, "off");
    const auto extended_rendered = extended->render(100);
    REQUIRE(extended_rendered);
    const auto extended_texts = row_texts(extended_rendered->lines);
    const std::vector<std::string> extended_rows = {
            "    xhigh     Extra-high reasoning (~32k tokens)",
            "    max       Maximum reasoning",
    };
    for (const auto& expected : extended_rows) {
        INFO("row " << expected);
        CHECK(std::ranges::find(extended_texts, expected) != extended_texts.end());
    }
}

TEST_CASE("ThinkingSelector renders the command hint in the theme's dim color",
        "[coding_agent][tui][thinking-selector][issue810][spec]") {
    auto theme = test_theme();
    std::optional<std::string> saved;
    auto selector = std::make_shared<coding_agent::tui::ThinkingSelectorComponent>(
            theme,
            test_keybindings(),
            "medium",
            std::vector<std::string>{"off", "medium", "high"},
            [](std::string) {},
            [] {},
            [&saved](std::string level) { saved = std::move(level); },
            std::nullopt);

    const auto rendered = selector->render(100);
    REQUIRE(rendered);
    // pi draws the affordance through `theme.fg("dim", ...)`, which the dark
    // theme resolves to `#666666`; `Muted` resolves to `#808080`.
    const std::string command_row = "  Enter to select · Ctrl+S to set as default · Escape/Ctrl+C to cancel";
    const auto expected = theme.foreground(coding_agent::tui::ThemeToken::Dim, command_row);
    const auto styled = std::ranges::find_if(
            rendered->lines, [&command_row](const std::string& line) { return row_text(line) == command_row; });
    REQUIRE(styled != rendered->lines.end());
    CHECK(styled->starts_with(expected));
}

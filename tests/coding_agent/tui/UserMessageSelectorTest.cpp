// The fork user-message selector (pi `user-message-selector.ts`): the overlay
// `/fork` opens. Its rows must fit every width the app can be split to — an
// over-wide row aborts the whole TUI through the render width guard (#790).

#include "coding_agent/tui/Theme.hpp"
#include "coding_agent/tui/UserMessageSelector.hpp"

#include <cch/tui/Keybindings.hpp>

#include <catch2/catch_test_macros.hpp>

#include "support/OverlayWidthBound.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace cch;

namespace {

[[nodiscard]] coding_agent::tui::LiveTheme test_theme() {
    return coding_agent::tui::LiveTheme(
            coding_agent::tui::builtin_dark_theme(), tui::TerminalColorCapability::TrueColor);
}

[[nodiscard]] std::shared_ptr<const tui::KeybindingRegistry> test_keybindings() {
    tui::KeybindingResolutionRequest request;
    request.definitions = tui::builtin_tui_keybinding_definitions();
    auto resolved = tui::resolve_keybindings(std::move(request));
    REQUIRE(resolved);
    return resolved->registry;
}

[[nodiscard]] coding_agent::tui::UserMessageSelectorComponent make_selector(
        const coding_agent::tui::LiveTheme& theme, std::vector<coding_agent::tui::UserForkItem> messages) {
    return coding_agent::tui::UserMessageSelectorComponent(
            theme,
            test_keybindings(),
            std::move(messages),
            std::optional<std::string>{"entry-2"},
            [](std::string) {},
            [] {},
            [] {});
}

} // namespace

TEST_CASE("UserMessageSelector bounds every row at narrow widths",
        "[coding_agent][tui][user-message-selector][issue790][spec]") {
    const auto theme = test_theme();
    auto populated = make_selector(theme,
            {
                    {.entry_id = "entry-1",
                            .text = "a long user message that must be truncated to the render width bound"},
                    {.entry_id = "entry-2", .text = "second message"},
            });

    for (const auto width : tests::kNarrowOverlayWidths) {
        const auto rendered = populated.render(width);
        REQUIRE(rendered);
        tests::check_all_lines_bounded(*rendered, width);
    }

    // The description row is the widest fixed row; it must survive the narrow
    // split a 116-column terminal produces.
    const auto narrow = populated.render(66);
    REQUIRE(narrow);
    CHECK(tests::over_wide_line(*narrow, 66).empty());
    const auto screen = [&] {
        std::string joined;
        for (const auto& line : narrow->lines) {
            joined += line;
            joined += '\n';
        }
        return joined;
    }();
    CHECK(screen.find("Select a user message") != std::string::npos);
    CHECK(screen.find("Fork from Message") != std::string::npos);

    auto empty = make_selector(theme, {});
    for (const auto width : tests::kNarrowOverlayWidths) {
        const auto rendered = empty.render(width);
        REQUIRE(rendered);
        tests::check_all_lines_bounded(*rendered, width);
    }
}

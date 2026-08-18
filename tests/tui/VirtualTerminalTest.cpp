// VirtualTerminal anchored absolute flow tests (issue #476, ADR 0041): the
// double IS the terminal, so start() anchors the buffer-to-screen origin at
// the (possibly seeded) shell cursor row instead of probing. These scenarios
// pin the seeded dirty-screen frame placement, the scroll mapping through the
// origin, and the clear-screen origin reset.

#include <cch/tui/Tui.hpp>
#include <cch/tui/VirtualTerminal.hpp>

#include <cch/support/Error.hpp>
#include <catch2/catch_test_macros.hpp>

#include <format>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace cch;

namespace {

/// A fixed-line component for renderer scenarios (the same shape as the
/// screen-state harness's LinesComponent).
class LinesComponent final : public tui::Component {
public:
    explicit LinesComponent(std::vector<std::string> lines) : lines_(std::move(lines)) {}

    void set_lines(std::vector<std::string> lines) {
        lines_ = std::move(lines);
        cache_valid_ = false;
    }

    [[nodiscard]] support::Expected<tui::RenderResult> render(std::size_t) override {
        if (cache_valid_) return tui::RenderResult{.lines = cached_};
        cached_ = lines_;
        cache_valid_ = true;
        return tui::RenderResult{.lines = lines_};
    }

    void invalidate() override {
        cache_valid_ = false;
    }

private:
    std::vector<std::string> lines_;
    std::vector<std::string> cached_;
    bool cache_valid_{false};
};

} // namespace

TEST_CASE(
    "VirtualTerminal anchors buffer row 0 at the seeded shell cursor row",
    "[tui][terminal][issue476]") {
    tui::VirtualTerminal terminal({.columns = 80, .rows = 10});
    REQUIRE(terminal.seed_shell_content(
        {"cmd one", "cmd two", "cmd three"},
        {.column = 0, .row = 3}));
    REQUIRE(terminal.start(
        [](std::string) -> support::ExpectedVoid { return {}; },
        [](tui::TerminalDimensions) -> support::ExpectedVoid { return {}; }));

    // The first frame row lands at the shell's cursor row: the seeded shell
    // content above stays intact and rows below the short frame are untouched.
    REQUIRE(terminal.set_cursor({.column = 0, .row = 0}));
    CHECK(terminal.cursor() == tui::CursorPosition{.column = 0, .row = 3});
    REQUIRE(terminal.write("frame"));
    CHECK(terminal.screen()[0] == "cmd one");
    CHECK(terminal.screen()[2] == "cmd three");
    CHECK(terminal.screen()[3] == "frame");
    CHECK(terminal.screen()[9].empty());

    // A buffer row past the visible bottom scrolls natively through the origin
    // mapping: buffer row 7 -> screen row 3 + 7 = 10 on a 10-row screen
    // scrolls one line, and the seeded top row enters the scrollback.
    REQUIRE(terminal.set_cursor({.column = 0, .row = 7}));
    CHECK(terminal.cursor() == tui::CursorPosition{.column = 0, .row = 9});
    CHECK(terminal.viewport_top() == 1);
    REQUIRE(terminal.scrollback().size() == 1);
    CHECK(terminal.scrollback().front() == "cmd one");
    REQUIRE(terminal.write("tail"));
    CHECK(terminal.screen()[9] == "tail");
    // Buffer row 0 is still visible (the first visible buffer row is
    // max(0, viewport_top - scroll_origin) = 0): it sits at screen row 2.
    CHECK(terminal.screen()[2] == "frame");
    REQUIRE(terminal.set_cursor({.column = 0, .row = 6}));
    CHECK(terminal.cursor() == tui::CursorPosition{.column = 0, .row = 8});
    REQUIRE(terminal.stop());
}

TEST_CASE(
    "VirtualTerminal resets the anchored origin on clear screen",
    "[tui][terminal][issue476]") {
    tui::VirtualTerminal terminal({.columns = 80, .rows = 10});
    REQUIRE(terminal.seed_shell_content({"cmd"}, {.column = 0, .row = 1}));
    REQUIRE(terminal.start(
        [](std::string) -> support::ExpectedVoid { return {}; },
        [](tui::TerminalDimensions) -> support::ExpectedVoid { return {}; }));

    REQUIRE(terminal.set_cursor({.column = 0, .row = 0}));
    CHECK(terminal.cursor() == tui::CursorPosition{.column = 0, .row = 1});

    // The resize full-redraw clears screen, homes, and clears scrollback
    // (pi's `\x1b[2J\x1b[H\x1b[3J`): the anchor resets to row 0 (ADR 0041).
    REQUIRE(terminal.clear_screen());
    CHECK(terminal.viewport_top() == 0);
    REQUIRE(terminal.set_cursor({.column = 0, .row = 0}));
    CHECK(terminal.cursor() == tui::CursorPosition{.column = 0, .row = 0});
    REQUIRE(terminal.write("frame"));
    CHECK(terminal.screen()[0] == "frame");
    CHECK(terminal.scrollback().empty());
    REQUIRE(terminal.stop());
}

TEST_CASE(
    "VirtualTerminal validates seeded shell content before start",
    "[tui][terminal][issue476]") {
    tui::VirtualTerminal terminal({.columns = 8, .rows = 3});
    const auto too_many = terminal.seed_shell_content(
        {"a", "b", "c", "d"},
        {.column = 0, .row = 0});
    REQUIRE_FALSE(too_many);
    const auto out_of_bounds = terminal.seed_shell_content({"a"}, {.column = 0, .row = 3});
    REQUIRE_FALSE(out_of_bounds);
    const auto too_wide = terminal.seed_shell_content(
        {"wider than eight"},
        {.column = 0, .row = 0});
    REQUIRE_FALSE(too_wide);
    const auto multiline = terminal.seed_shell_content(
        {std::string{"a\nb"}},
        {.column = 0, .row = 0});
    REQUIRE_FALSE(multiline);

    REQUIRE(terminal.seed_shell_content({"a"}, {.column = 0, .row = 1}));
    REQUIRE(terminal.start(
        [](std::string) -> support::ExpectedVoid { return {}; },
        [](tui::TerminalDimensions) -> support::ExpectedVoid { return {}; }));
    const auto after_start = terminal.seed_shell_content({"b"}, {.column = 0, .row = 0});
    REQUIRE_FALSE(after_start);
    CHECK(
        after_start.error().message ==
        "Virtual Terminal shell content must be seeded before start");
    REQUIRE(terminal.stop());
}

TEST_CASE(
    "VirtualTerminal flows the first Tui frame from the seeded shell cursor row",
    "[tui][terminal][issue476]") {
    tui::VirtualTerminal terminal({.columns = 80, .rows = 10});
    REQUIRE(terminal.seed_shell_content(
        {"cmd one", "cmd two", "cmd three"},
        {.column = 0, .row = 3}));
    tui::Tui tui(terminal);
    auto lines = std::make_unique<LinesComponent>(std::vector<std::string>{"line 00", "line 01"});
    auto* pointer = lines.get();
    REQUIRE(tui.add_child(std::move(lines)));
    REQUIRE(tui.start());
    REQUIRE(tui.render());

    // The first render flows from the cursor row (pi TuiMainScreen
    // fullRender(false)): shell content above the frame stays intact and the
    // rows below the short buffer are never overwritten (issue #476).
    const auto& boot = terminal.screen();
    CHECK(boot[0] == "cmd one");
    CHECK(boot[1] == "cmd two");
    CHECK(boot[2] == "cmd three");
    CHECK(boot[3].starts_with("line 00"));
    CHECK(boot[4].starts_with("line 01"));
    for (std::size_t row = 5; row < boot.size(); ++row) {
        CHECK(boot[row].empty());
    }
    CHECK(terminal.scrollback().empty());

    // Growth past one screen scrolls through the origin mapping: the shell
    // content and the frame's first rows advance into the native scrollback.
    std::vector<std::string> grown;
    for (std::size_t index = 0; index < 12; ++index) {
        grown.push_back(std::format("line {:02}", index));
    }
    pointer->set_lines(grown);
    REQUIRE(tui.render());
    const auto& scrollback = terminal.scrollback();
    REQUIRE(scrollback.size() == 5);
    CHECK(scrollback[0] == "cmd one");
    CHECK(scrollback[1] == "cmd two");
    CHECK(scrollback[2] == "cmd three");
    CHECK(scrollback[3].starts_with("line 00"));
    CHECK(scrollback[4].starts_with("line 01"));
    CHECK(terminal.viewport_top() == 5);
    // The visible screen shows buffer rows 2..11 (the first visible buffer row
    // is viewport_top - scroll_origin = 5 - 3 = 2).
    const auto& grown_screen = terminal.screen();
    for (std::size_t row = 0; row < grown_screen.size(); ++row) {
        CHECK(grown_screen[row].starts_with(std::format("line {:02}", row + 2)));
    }

    // clear_screen homes and re-anchors at row 0 (ADR 0041).
    REQUIRE(tui.clear_screen());
    CHECK(terminal.viewport_top() == 0);
    // The 12-line buffer overflows the 10-row screen from row 0, so the first
    // two lines scroll into scrollback and the tail stays on screen.
    REQUIRE(tui.render());
    const auto& rewritten = terminal.scrollback();
    REQUIRE(rewritten.size() == 2);
    CHECK(rewritten[0].starts_with("line 00"));
    CHECK(rewritten[1].starts_with("line 01"));
    CHECK(terminal.screen().front().starts_with("line 02"));
    CHECK(terminal.screen().back().starts_with("line 11"));
    REQUIRE(tui.stop());
}

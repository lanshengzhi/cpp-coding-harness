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

TEST_CASE("VirtualTerminal anchors buffer row 0 at the seeded shell cursor row", "[tui][terminal][issue476][spec]") {
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

TEST_CASE("VirtualTerminal resets the anchored origin on clear screen", "[tui][terminal][issue476][spec]") {
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

TEST_CASE("VirtualTerminal validates seeded shell content before start", "[tui][terminal][issue476][spec]") {
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

TEST_CASE("VirtualTerminal flows the first Tui frame from the seeded shell cursor row",
        "[tui][terminal][issue476][spec]") {
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

TEST_CASE("VirtualTerminal scroll margins constrain viewport scrolling", "[tui][terminal][issue598][spec]") {
    tui::VirtualTerminal terminal({.columns = 80, .rows = 10});
    REQUIRE(terminal.start([](std::string) -> support::ExpectedVoid { return {}; },
            [](tui::TerminalDimensions) -> support::ExpectedVoid { return {}; }));

    // Configure rows 0..6 as the scrolling viewport.
    REQUIRE(terminal.set_scroll_margins(0, 6));

    // Write lines into the viewport exceeding viewport height (7 lines).
    // Write 10 viewport lines (rows 0..9).
    for (std::size_t i = 0; i < 10; ++i) {
        REQUIRE(terminal.set_cursor({.column = 0, .row = i}));
        REQUIRE(terminal.write(std::format("vp line {:02}", i)));
    }

    // 10 lines in a 7-line viewport causes 3 scrolls.
    // Lines 0, 1, 2 should have scrolled into scrollback.
    const auto& scrollback = terminal.scrollback();
    REQUIRE(scrollback.size() == 3);
    CHECK(scrollback[0] == "vp line 00");
    CHECK(scrollback[1] == "vp line 01");
    CHECK(scrollback[2] == "vp line 02");

    // Viewport rows 0..6 show the latest lines 3..9
    const auto& screen = terminal.screen();
    for (std::size_t i = 0; i < 7; ++i) {
        CHECK(screen[i] == std::format("vp line {:02}", i + 3));
    }

    // Now test linefeed streaming inside viewport:
    // Move to bottom of viewport (row 9 in buffer sits at screen row 6) and emit newlines
    REQUIRE(terminal.set_cursor({.column = 0, .row = 9}));
    REQUIRE(terminal.write("\nvp line 10\nvp line 11"));

    // 2 more lines scrolled into scrollback
    REQUIRE(terminal.scrollback().size() == 5);
    CHECK(terminal.scrollback()[3] == "vp line 03");
    CHECK(terminal.scrollback()[4] == "vp line 04");

    REQUIRE(terminal.stop());
}

TEST_CASE("VirtualTerminal validates scroll margin regions", "[tui][terminal][issue598][spec]") {
    tui::VirtualTerminal terminal({.columns = 80, .rows = 10});
    REQUIRE(terminal.start([](std::string) -> support::ExpectedVoid { return {}; },
            [](tui::TerminalDimensions) -> support::ExpectedVoid { return {}; }));

    REQUIRE(terminal.set_scroll_margins(0, 6));

    // Invalid margin parameters (top > bottom or bottom >= rows); an equal
    // top and bottom is a valid one-row scroll region (#611).
    auto invalid_margins1 = terminal.set_scroll_margins(6, 5);
    REQUIRE_FALSE(invalid_margins1);
    CHECK(invalid_margins1.error().message == "Virtual Terminal scroll margins are invalid");

    auto invalid_margins2 = terminal.set_scroll_margins(0, 10);
    REQUIRE_FALSE(invalid_margins2);
    CHECK(invalid_margins2.error().message == "Virtual Terminal scroll margins are invalid");

    auto invalid_margins3 = terminal.set_scroll_margins(8, 6);
    REQUIRE_FALSE(invalid_margins3);
    CHECK(invalid_margins3.error().message == "Virtual Terminal scroll margins are invalid");

    REQUIRE(terminal.stop());
}

TEST_CASE("VirtualTerminal reset_scroll_margins restores full-screen scrolling", "[tui][terminal][issue598][spec]") {
    tui::VirtualTerminal terminal({.columns = 80, .rows = 10});
    REQUIRE(terminal.start([](std::string) -> support::ExpectedVoid { return {}; },
            [](tui::TerminalDimensions) -> support::ExpectedVoid { return {}; }));

    // Set scroll margins 0..6
    REQUIRE(terminal.set_scroll_margins(0, 6));
    // Reset scroll margins: restores full screen 0..9
    REQUIRE(terminal.reset_scroll_margins());

    // Check output contains DECSTBM reset
    bool found_reset = false;
    for (const auto& out : terminal.output()) {
        if (out == "\x1b[1;10r") {
            found_reset = true;
            break;
        }
    }
    CHECK(found_reset);

    // Now write 15 lines from row 0 without margins
    REQUIRE(terminal.clear_screen());
    for (std::size_t i = 0; i < 15; ++i) {
        REQUIRE(terminal.set_cursor({.column = 0, .row = i}));
        REQUIRE(terminal.write(std::format("full line {:02}", i)));
    }

    // With full-screen scrolling, 15 lines in 10 rows causes 5 scrolls
    REQUIRE(terminal.scrollback().size() == 5);
    for (std::size_t i = 0; i < 5; ++i) {
        CHECK(terminal.scrollback()[i] == std::format("full line {:02}", i));
    }
    // Screen rows 0..9 show lines 5..14
    for (std::size_t i = 0; i < 10; ++i) {
        CHECK(terminal.screen()[i] == std::format("full line {:02}", i + 5));
    }

    REQUIRE(terminal.stop());
}

TEST_CASE("VirtualTerminal starts regular TUI rendering using relative line flow without DSR or clearing scrollback",
        "[tui][terminal][issue983][issue985][spec]") {
    tui::VirtualTerminal terminal({.columns = 40, .rows = 10});
    REQUIRE(terminal.seed_shell_content(
            {"user@box:~$ ls", "file1.txt  file2.txt", "user@box:~$ echo hi"}, {.column = 0, .row = 3}));
    tui::Tui tui(terminal);
    auto lines = std::make_unique<LinesComponent>(std::vector<std::string>{"initial line 1", "initial line 2"});
    REQUIRE(tui.add_child(std::move(lines)));
    REQUIRE(tui.start());
    REQUIRE(tui.render());

    // Relative rendering starts at cursor row 3 without clearing screen or scrollback
    const auto& screen = terminal.screen();
    CHECK(screen[0] == "user@box:~$ ls");
    CHECK(screen[1] == "file1.txt  file2.txt");
    CHECK(screen[2] == "user@box:~$ echo hi");
    CHECK(screen[3].starts_with("initial line 1"));
    CHECK(screen[4].starts_with("initial line 2"));
    CHECK(terminal.scrollback().empty());

    // Stop cancels deferred wrap with a space, then moves below the frame.
    // The two rendered rows start at shell row 3, so CRLF leaves the cursor 3
    // rows below its pre-stop position.
    REQUIRE(tui.stop());
    CHECK(terminal.cursor().row == 7);
}

TEST_CASE("Tui starts regular rendering on dirty screens at physical rows 1 and 4 without CPR",
        "[tui][terminal][issue983][issue985][spec]") {
    // Frozen pi TuiMainScreen flows rendered lines relative to whatever shell
    // cursor it inherits: content above the cursor stays untouched, the frame
    // starts at the cursor row, and stop() leaves the cursor below the frame
    // (space, line-diff move, CRLF). No CPR reply is available to any case.
    const auto render_from = [](std::size_t seed_row, std::size_t expected_stop_row) {
        tui::VirtualTerminal terminal({.columns = 24, .rows = 8});
        std::vector<std::string> shell_lines;
        for (std::size_t row = 0; row < seed_row; ++row)
            shell_lines.push_back(std::format("shell row {}", row));
        REQUIRE(terminal.seed_shell_content(shell_lines, {.column = 0, .row = seed_row}));

        tui::Tui tui(terminal);
        auto lines = std::make_unique<LinesComponent>(std::vector<std::string>{"alpha", "beta"});
        REQUIRE(tui.add_child(std::move(lines)));
        REQUIRE(tui.start());
        REQUIRE(tui.render());

        const auto& screen = terminal.screen();
        for (std::size_t row = 0; row < seed_row; ++row)
            CHECK(screen[row] == std::format("shell row {}", row));
        CHECK(screen[seed_row].starts_with("alpha"));
        CHECK(screen[seed_row + 1].starts_with("beta"));
        CHECK(terminal.scrollback().empty());
        // The frame flows from the shell cursor; it never clears the screen
        // or scrollback and never depends on a cursor-position reply.
        for (const auto& chunk : terminal.output()) {
            CHECK(chunk.find("\x1b[6n") == std::string::npos);
            CHECK(chunk.find("\x1b[2J") == std::string::npos);
            CHECK(chunk.find("\x1b[3J") == std::string::npos);
        }

        REQUIRE(tui.stop());
        CHECK(terminal.cursor().row == expected_stop_row);
    };

    render_from(1, 5);
    render_from(4, 7);
}

TEST_CASE("VirtualTerminal atomic stop rejects unstarted terminal or non-alternate screen state",
        "[tui][terminal][atomic][stop][virtual][issue1019][spec]") {
    tui::VirtualTerminal terminal({.columns = 40, .rows = 10});

    SECTION("rejects unstarted terminal") {
        auto stop_res = terminal.stop(cch::tui::TerminalStopOptions{.final_alternate_restore = true});
        REQUIRE_FALSE(stop_res.has_value());
        CHECK(stop_res.error().code == cch::support::ErrorCode::Validation);
        CHECK(stop_res.error().message.find("must be started") != std::string::npos);
    }

    SECTION("rejects started terminal not in alternate screen") {
        REQUIRE(terminal.start([](std::string) -> cch::support::ExpectedVoid { return {}; },
                [](cch::tui::TerminalDimensions) -> cch::support::ExpectedVoid { return {}; }));

        auto stop_res = terminal.stop(cch::tui::TerminalStopOptions{.final_alternate_restore = true});
        REQUIRE_FALSE(stop_res.has_value());
        CHECK(stop_res.error().code == cch::support::ErrorCode::Validation);
        CHECK(stop_res.error().message.find("not in alternate screen") != std::string::npos);

        REQUIRE(terminal.stop());
    }
}

TEST_CASE("VirtualTerminal atomic stop flushes open synchronized update and restores autowrap",
        "[tui][terminal][atomic][stop][virtual][issue1019][spec]") {
    tui::VirtualTerminal terminal({.columns = 40, .rows = 10});
    REQUIRE(terminal.start([](std::string) -> cch::support::ExpectedVoid { return {}; },
            [](cch::tui::TerminalDimensions) -> cch::support::ExpectedVoid { return {}; }));
    REQUIRE(terminal.set_alternate_screen(true));
    REQUIRE(terminal.set_autowrap(false));
    REQUIRE(terminal.begin_synchronized_update());
    REQUIRE(terminal.write("VIRTUAL_PAYLOAD_TEST"));

    REQUIRE(terminal.stop(cch::tui::TerminalStopOptions{.final_alternate_restore = true}));

    auto join_output = [](const std::vector<std::string>& out) {
        std::string res;
        for (const auto& s : out)
            res += s;
        return res;
    };
    const auto full_output = join_output(terminal.output());

    CHECK(full_output.find("\x1b[?2026l") != std::string::npos);
    CHECK(full_output.find("\x1b[?7h") != std::string::npos);
    CHECK(full_output.find("\x1b[?2026h\x1b[?1049l\x1b[?25h\x1b[?2026l") != std::string::npos);
    CHECK(terminal.modes().autowrap == true);
    CHECK(terminal.modes().alternate_screen == false);
    CHECK(terminal.modes().cursor_visible == true);

    // Sync depth and modes restored: subsequent start succeeds
    auto follow_start = terminal.start([](std::string) -> cch::support::ExpectedVoid { return {}; },
            [](cch::tui::TerminalDimensions) -> cch::support::ExpectedVoid { return {}; });
    REQUIRE(follow_start.has_value());
    REQUIRE(terminal.stop());
}

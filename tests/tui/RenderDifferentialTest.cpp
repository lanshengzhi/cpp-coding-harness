#include <cch/tui/Overlay.hpp>
#include <cch/tui/Text.hpp>
#include <cch/tui/Tui.hpp>
#include <cch/tui/VirtualTerminal.hpp>

#include <cch/support/Error.hpp>
#include "tui/RenderPipeline.hpp"
#include "tui/TuiTestHooks.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <charconv>
#include <format>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace {

/// A Component that returns a fixed set of lines on each render.
class ManualTuiScheduler final : public cch::tui::TuiScheduler {
public:
    [[nodiscard]] std::chrono::steady_clock::time_point now() const noexcept override { return now_; }
    void post(cch::tui::TuiScheduledWork work) override { posted_.push_back(std::move(work)); }
    void schedule(std::chrono::steady_clock::duration delay, cch::tui::TuiScheduledWork work) override {
        scheduled_delay_ = delay;
        scheduled_ = std::move(work);
        ++schedule_count_;
    }
    void cancel() noexcept override { scheduled_ = nullptr; }

    std::chrono::steady_clock::duration scheduled_delay_{};
    std::size_t schedule_count_{0};

private:
    std::chrono::steady_clock::time_point now_{};
    cch::tui::TuiScheduledWork scheduled_;
    std::vector<cch::tui::TuiScheduledWork> posted_;
};

class FixedTextComponent final : public cch::tui::Component {
public:
    explicit FixedTextComponent(std::vector<std::string> lines) : lines_(std::move(lines)) {}

    void set_lines(std::vector<std::string> lines) {
        lines_ = std::move(lines);
        cache_valid_ = false;
    }

    [[nodiscard]] cch::support::Expected<cch::tui::RenderResult> render(std::size_t) override {
        if (cache_valid_) return cch::tui::RenderResult{.lines = cached_};
        cached_ = lines_;
        cache_valid_ = true;
        return cch::tui::RenderResult{.lines = lines_};
    }

    void invalidate() override { cache_valid_ = false; }

private:
    std::vector<std::string> lines_;
    std::vector<std::string> cached_;
    bool cache_valid_{false};
};

/// A focusable that reports a fixed buffer-relative cursor row and renders no
/// lines, for pinning the IME cursor viewport clamp over scrolled content.
class CursorReportingComponent final : public cch::tui::Component, public cch::tui::Focusable {
public:
    explicit CursorReportingComponent(std::size_t cursor_row) : cursor_row_(cursor_row) {}

    void set_row(std::size_t row) { cursor_row_ = row; }

    [[nodiscard]] cch::support::Expected<cch::tui::RenderResult> render(std::size_t) override {
        return cch::tui::RenderResult{};
    }

    void invalidate() override {}

    void set_focused(bool focused) override { focused_ = focused; }

    [[nodiscard]] bool focused() const override { return focused_; }

    [[nodiscard]] std::optional<cch::tui::CursorPosition> cursor_location() const override {
        return cch::tui::CursorPosition{.column = 0, .row = cursor_row_};
    }

private:
    std::size_t cursor_row_;
    bool focused_{false};
};

/// Distinct single-byte-width rows, so a frame's addressed rows are visible in
/// the terminal's recorded cursor moves.
[[nodiscard]] std::vector<std::string> session_rows(std::size_t count) {
    std::vector<std::string> lines;
    lines.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        lines.push_back(std::format("row-{:04d}", index));
    }
    return lines;
}

/// A Terminal with a bounded undrained output queue: an escape sequence is
/// admitted while the queue has room and refused with a typed Busy afterwards,
/// the way ProcessTerminal refuses a backed-up queue. `drain()` models the
/// terminal consuming what it holds, so a resumed frame can make progress
} // namespace

TEST_CASE("First render writes visible content without clearing scrollback", "[tui][render][issue49][compat-pi]") {
    cch::tui::VirtualTerminal terminal({.columns = 6, .rows = 3});
    cch::tui::Tui tui(terminal);
    REQUIRE(tui.add_child(std::make_unique<cch::tui::Text>("hello", 0, 0)));
    REQUIRE(tui.start());

    // First render should NOT call clear_screen
    REQUIRE(tui.render());
    CHECK_FALSE(terminal.check_clear_screen_called());

    // Content is visible
    const std::vector<std::string> expected_screen{"hello ", "", ""};
    CHECK(terminal.screen() == expected_screen);
}

TEST_CASE("Normal updates begin at first changed visible line", "[tui][render][issue49][compat-pi]") {
    cch::tui::VirtualTerminal terminal({.columns = 10, .rows = 5});
    cch::tui::Tui tui(terminal);

    auto text = std::make_unique<cch::tui::Text>("line one\nline two\nline thr", 0, 0);
    auto* text_ptr = text.get();
    REQUIRE(tui.add_child(std::move(text)));
    REQUIRE(tui.start());
    REQUIRE(tui.render());

    // Reset clear_screen flag since first render path doesn't call it
    (void)terminal.check_clear_screen_called();

    // Change only the last line (stays within width at 10 columns)
    text_ptr->set_text("line one\nline two\nline chg");
    REQUIRE(tui.render());

    // Should NOT have cleared full screen for a trailing change
    CHECK_FALSE(terminal.check_clear_screen_called());

    // Screen should show updated content
    const std::vector<std::string> expected_screen{
            "line one  ",
            "line two  ",
            "line chg  ",
            "",
            "",
    };
    CHECK(terminal.screen() == expected_screen);
}

TEST_CASE("Tui pads composited overlay rows by visible width before overwriting the screen",
        "[tui][render][overlay][issue794][spec]") {
    cch::tui::VirtualTerminal terminal({.columns = 12, .rows = 3});
    cch::tui::Tui tui(terminal);
    auto base = std::make_unique<FixedTextComponent>(std::vector<std::string>{"old content"});
    auto* base_pointer = base.get();
    REQUIRE(tui.add_child(std::move(base)));
    REQUIRE(tui.start());
    REQUIRE(tui.render());

    base_pointer->set_lines({""});
    cch::tui::OverlayOptions options;
    options.col = 0.0;
    options.row = 0.0;
    auto overlay = std::make_unique<cch::tui::Overlay>(options);
    REQUIRE(overlay->add_child(
            std::make_unique<FixedTextComponent>(std::vector<std::string>{"\x1b[38;5;196mX\x1b[39m"})));
    REQUIRE(tui.add_overlay(std::move(overlay)));
    REQUIRE(tui.render());

    CHECK(terminal.screen()[0] == "X           ");
}

TEST_CASE("Shrinking content clears stale rows below new content", "[tui][render][issue49][compat-pi]") {
    cch::tui::VirtualTerminal terminal({.columns = 5, .rows = 4});
    cch::tui::Tui tui(terminal);

    auto text = std::make_unique<cch::tui::Text>("aaa\nbbb\nccc\nddd", 0, 0);
    auto* text_ptr = text.get();
    REQUIRE(tui.add_child(std::move(text)));
    REQUIRE(tui.start());
    REQUIRE(tui.render());

    // Reset tracking
    (void)terminal.check_clear_screen_called();

    // Content shrinks from 4 lines to 2
    text_ptr->set_text("aaa\nnew");
    REQUIRE(tui.render());

    // Should NOT have cleared full screen for a simple shrink
    CHECK_FALSE(terminal.check_clear_screen_called());

    // Screen shows new content, rows 2-3 are cleared (spaces)
    const auto& screen = terminal.screen();
    REQUIRE(screen.size() == 4);
    CHECK(screen[0] == "aaa  ");
    CHECK(screen[1] == "new  ");
    // Rows 2 and 3 should have been cleared to spaces
    CHECK(screen[2] == "     ");
    CHECK(screen[3] == "     ");
}

TEST_CASE("Width change triggers full redraw with clear screen", "[tui][render][issue49][compat-pi]") {
    cch::tui::VirtualTerminal terminal({.columns = 5, .rows = 3});
    cch::tui::Tui tui(terminal);
    REQUIRE(tui.add_child(std::make_unique<cch::tui::Text>("hello", 0, 0)));
    REQUIRE(tui.start());
    REQUIRE(tui.render());

    // Change dimensions by injecting a resize (this calls invalidate internally)
    REQUIRE(terminal.inject_resize({.columns = 10, .rows = 3}));

    // Render should detect width change and do a full redraw with clear_screen
    REQUIRE(tui.render());
    CHECK(terminal.check_clear_screen_called());
    CHECK(terminal.check_clear_scrollback_called());

    // Content should be re-rendered at the new width
    const std::vector<std::string> expected_screen{"hello     ", "", ""};
    CHECK(terminal.screen() == expected_screen);
}

TEST_CASE("Resize full redraw emits the pi-exact clear-screen and scrollback bytes",
        "[tui][render][issue49][issue435][compat-pi]") {
    cch::tui::VirtualTerminal terminal({.columns = 8, .rows = 3});
    cch::tui::Tui tui(terminal);
    REQUIRE(tui.add_child(std::make_unique<cch::tui::Text>("top\nmiddle\nbottom", 0, 0)));
    REQUIRE(tui.start());
    REQUIRE(tui.render());

    // A height change reflows from a clean screen; the clear is emitted inside
    // the synchronized-update wrapper as pi's fullRender(true) does.
    REQUIRE(terminal.inject_resize({.columns = 8, .rows = 4}));
    REQUIRE(tui.render());

    const auto& output = terminal.output();
    const auto clear = std::find(output.begin(), output.end(), "\x1b[2J\x1b[H\x1b[3J");
    REQUIRE(clear != output.end());
    // The clear-scrollback (`\x1b[3J`) is part of the same emitted clear.
    CHECK(clear->find("\x1b[3J") != std::string::npos);
    // Synchronized output still wraps the render atomically (pi fullRender
    // begins `\x1b[?2026h` and ends `\x1b[?2026l`).
    CHECK(output.front() == "\x1b[?2026h");
    CHECK(output.back() == "\x1b[?2026l");
}

TEST_CASE("Tui clamps the IME cursor to the visible viewport over scrolled content",
        "[tui][render][issue49][issue435][compat-pi]") {
    cch::tui::VirtualTerminal terminal({.columns = 8, .rows = 3});
    cch::tui::Tui tui(terminal);
    auto cursor = std::make_unique<CursorReportingComponent>(4);
    auto* cursor_ptr = cursor.get();
    REQUIRE(tui.add_child(std::move(cursor)));
    REQUIRE(tui.add_child(std::make_unique<cch::tui::Text>("one\ntwo\nthree\nfour\nfive", 0, 0)));
    REQUIRE(tui.start());
    REQUIRE(tui.set_focus(cursor_ptr));
    REQUIRE(tui.render());

    // Five content lines on a three-row viewport: the viewport top is at
    // buffer row 2 (rows 2..4 visible). The cursor at buffer row 4 is inside
    // the viewport, so it is positioned at screen row 2 (pi
    // positionHardwareCursor relative to the viewport).
    REQUIRE(terminal.viewport_top() == 2);
    const cch::tui::CursorPosition expected_visible{.column = 0, .row = 2};
    CHECK(terminal.cursor() == expected_visible);

    // A cursor above the viewport is clamped to the viewport top instead of
    // scrolling the terminal.
    cursor_ptr->set_row(1);
    tui.invalidate();
    REQUIRE(tui.render());
    const cch::tui::CursorPosition expected_clamped{.column = 0, .row = 0};
    CHECK(terminal.cursor() == expected_clamped);
}

TEST_CASE("Changes at the viewport top diff in place without clearing", "[tui][render][issue49][issue435][compat-pi]") {
    cch::tui::VirtualTerminal terminal({.columns = 5, .rows = 3});
    cch::tui::Tui tui(terminal);

    auto text = std::make_unique<cch::tui::Text>("aaa\nbbb", 0, 0);
    auto* text_ptr = text.get();
    REQUIRE(tui.add_child(std::move(text)));
    REQUIRE(tui.start());
    REQUIRE(tui.render());

    // Reset tracking
    (void)terminal.check_clear_screen_called();

    // Change the first line
    text_ptr->set_text("ccc\nbbb");
    REQUIRE(tui.render());

    // pi: a change at the viewport top (firstChanged == viewportTop) is
    // reached with line flow — no full redraw.
    CHECK_FALSE(terminal.check_clear_screen_called());

    const std::vector<std::string> expected_screen{"ccc  ", "bbb  ", ""};
    CHECK(terminal.screen() == expected_screen);
}

TEST_CASE("Changes above the tracked viewport trigger a full redraw", "[tui][render][issue49][issue435][compat-pi]") {
    cch::tui::VirtualTerminal terminal({.columns = 5, .rows = 3});
    cch::tui::Tui tui(terminal);

    auto text = std::make_unique<cch::tui::Text>("aaa\nbbb\nccc\nddd\neee", 0, 0);
    auto* text_ptr = text.get();
    REQUIRE(tui.add_child(std::move(text)));
    REQUIRE(tui.start());
    REQUIRE(tui.render());

    // Five lines on a three-row viewport: the top two scrolled into the
    // terminal's native scrollback and the viewport top is at buffer row 2.
    const std::vector<std::string> expected_scrollback{"aaa  ", "bbb  "};
    CHECK(terminal.scrollback() == expected_scrollback);
    const std::vector<std::string> expected_screen{"ccc  ", "ddd  ", "eee  "};
    CHECK(terminal.screen() == expected_screen);
    (void)terminal.check_clear_screen_called();

    // Changing a line above the tracked viewport cannot be reached with line
    // flow: the renderer reflows from a clean screen, clearing screen and
    // scrollback together (pi firstChanged < viewportTop).
    text_ptr->set_text("aaa\nXbb\nccc\nddd\neee");
    REQUIRE(tui.render());
    CHECK(terminal.check_clear_screen_called());
    CHECK(terminal.check_clear_scrollback_called());
    const std::vector<std::string> reflowed_scrollback{"aaa  ", "Xbb  "};
    CHECK(terminal.scrollback() == reflowed_scrollback);
    const std::vector<std::string> reflowed_screen{"ccc  ", "ddd  ", "eee  "};
    CHECK(terminal.screen() == reflowed_screen);
}

TEST_CASE("Supported synchronized output wraps a render atomically", "[tui][render][issue49][compat-pi]") {
    cch::tui::VirtualTerminal terminal({.columns = 4, .rows = 2});
    cch::tui::Tui tui(terminal);
    REQUIRE(tui.add_child(std::make_unique<cch::tui::Text>("hi", 0, 0)));
    REQUIRE(tui.start());
    REQUIRE(tui.render());

    // Output should be wrapped in sync markers
    const auto& output = terminal.output();
    REQUIRE(output.size() >= 3);
    CHECK(output.front() == "\x1b[?2026h");
    CHECK(output.back() == "\x1b[?2026l");
}

TEST_CASE("Render requests coalesce until render and include resize", "[tui][render][issue58][compat-pi]") {
    cch::tui::VirtualTerminal terminal({.columns = 7, .rows = 2});
    ManualTuiScheduler scheduler;
    cch::tui::Tui tui(terminal);
    REQUIRE(tui.add_child(std::make_unique<cch::tui::Text>("initial", 0, 0)));
    tui.set_scheduler(&scheduler);
    REQUIRE(tui.start());
    REQUIRE(tui.render());

    tui.invalidate();
    tui.invalidate();
    CHECK(scheduler.schedule_count_ == 2);
    REQUIRE(tui.render());

    tui.invalidate();
    CHECK(scheduler.schedule_count_ == 3);
    REQUIRE(tui.render());
    REQUIRE(terminal.inject_resize({.columns = 8, .rows = 2}));
    CHECK(scheduler.schedule_count_ == 4);
}

TEST_CASE("Repeated invalidate coalesces without losing latest state", "[tui][render][issue49][compat-pi]") {
    cch::tui::VirtualTerminal terminal({.columns = 7, .rows = 2});
    cch::tui::Tui tui(terminal);
    REQUIRE(tui.add_child(std::make_unique<cch::tui::Text>("initial", 0, 0)));
    REQUIRE(tui.start());

    // Multiple invalidates before render should still produce correct final state
    tui.invalidate();
    tui.invalidate();
    tui.invalidate();
    REQUIRE(tui.render());

    const std::vector<std::string> expected_screen{"initial", ""};
    CHECK(terminal.screen() == expected_screen);
}

TEST_CASE("Empty content renders zero lines correctly", "[tui][render][issue49][compat-pi]") {
    cch::tui::VirtualTerminal terminal({.columns = 4, .rows = 2});
    cch::tui::Tui tui(terminal);
    // No children means no content
    REQUIRE(tui.start());

    REQUIRE(tui.render());

    // Screen should be empty
    const auto& screen = terminal.screen();
    REQUIRE(screen.size() == 2);
    CHECK(screen[0].empty());
    CHECK(screen[1].empty());
}

TEST_CASE("Shrink to empty content clears stale rows", "[tui][render][issue49][compat-pi]") {
    cch::tui::VirtualTerminal terminal({.columns = 4, .rows = 2});
    cch::tui::Tui tui(terminal);

    auto text = std::make_unique<cch::tui::Text>("hello", 0, 0);
    auto* text_ptr = text.get();
    REQUIRE(tui.add_child(std::move(text)));
    REQUIRE(tui.start());
    REQUIRE(tui.render());

    // Change to empty text
    text_ptr->set_text("");
    text_ptr->invalidate();
    REQUIRE(tui.render());

    // Shrink to zero is reached with line flow (pi): the stale rows are
    // cleared in place, so the screen holds blank cells instead of the old
    // content.
    const auto& screen = terminal.screen();
    REQUIRE(screen.size() == 2);
    CHECK(screen[0] == "    ");
    CHECK(screen[1] == "    ");
}

TEST_CASE("Viewport height change triggers a full redraw with clear", "[tui][render][issue49][issue435][compat-pi]") {
    cch::tui::VirtualTerminal terminal({.columns = 6, .rows = 2});
    cch::tui::Tui tui(terminal);
    REQUIRE(tui.add_child(std::make_unique<cch::tui::Text>("line1\nline2", 0, 0)));
    REQUIRE(tui.start());
    REQUIRE(tui.render());
    (void)terminal.check_clear_screen_called();

    // Resize height to 3 (width unchanged)
    REQUIRE(terminal.inject_resize({.columns = 6, .rows = 3}));

    // pi: a height change reflows from a clean screen — the full redraw
    // clears the screen and the terminal's scroll history together.
    REQUIRE(tui.render());
    CHECK(terminal.check_clear_screen_called());
    CHECK(terminal.check_clear_scrollback_called());

    // Screen should show all content in the new height (no wrapping at 6 cols).
    const auto& screen = terminal.screen();
    REQUIRE(screen.size() == 3);
    CHECK(screen[0] == "line1 ");
    CHECK(screen[1] == "line2 ");
    CHECK(screen[2].empty());
    CHECK(screen[2].size() == 0);
}

TEST_CASE("Pending render request is consumed on render call", "[tui][render][issue49][compat-pi]") {
    cch::tui::VirtualTerminal terminal({.columns = 4, .rows = 2});
    cch::tui::Tui tui(terminal);

    auto text = std::make_unique<cch::tui::Text>("hi", 0, 0);
    auto* text_ptr = text.get();
    REQUIRE(tui.add_child(std::move(text)));
    REQUIRE(tui.start());
    REQUIRE(tui.render());

    // Change content and invalidate
    text_ptr->set_text("bye");
    tui.invalidate();

    // Pending render is consumed and produces the latest content
    REQUIRE(tui.render());
    const std::vector<std::string> expected_screen{"bye ", ""};
    CHECK(terminal.screen() == expected_screen);
}

TEST_CASE("A Preview Frame that changes nothing prepares no composed rows", "[tui][render][issue711][spec]") {
    cch::tui::VirtualTerminal terminal({.columns = 10, .rows = 6});
    cch::tui::Tui tui(terminal);

    REQUIRE(tui.add_child(std::make_unique<cch::tui::Text>("alpha\nbravo\ncharlie", 0, 0)));
    REQUIRE(tui.start());
    REQUIRE(tui.render());
    CHECK(cch::tui::detail::testing::frame_prepare_call_count(tui) == 3);
    const std::vector<std::string> first_screen = terminal.screen();

    // No view change: every composed row is byte-identical, so the frame
    // reuses the previous finalized rows instead of re-preparing them.
    REQUIRE(tui.render());
    CHECK(cch::tui::detail::testing::frame_prepare_call_count(tui) == 0);
    CHECK(terminal.screen() == first_screen);
}

TEST_CASE("A spinner update rewrites only its row and keeps the frozen viewport", "[tui][render][issue984][spec]") {
    cch::tui::VirtualTerminal terminal({.columns = 8, .rows = 4});
    cch::tui::Tui tui(terminal);

    auto text = std::make_unique<FixedTextComponent>(session_rows(6));
    auto* text_ptr = text.get();
    REQUIRE(tui.add_child(std::move(text)));
    REQUIRE(tui.start());
    REQUIRE(tui.render());

    // Six lines on a four-row viewport scrolled the first two into the
    // terminal's history; the viewport holds buffer rows 2..5.
    const std::vector<std::string> frozen_scrollback{"row-0000", "row-0001"};
    CHECK(terminal.scrollback() == frozen_scrollback);
    const std::vector<std::string> frozen_viewport{"row-0002", "row-0003", "row-0004", "row-0005"};
    CHECK(terminal.screen() == frozen_viewport);
    (void)terminal.check_clear_screen_called();

    // A spinner tick on the last visible row (pi firstChanged == viewportTop
    // + 3) diffs in place: the frozen history and the rows above the change
    // are never rewritten, and no full redraw is requested.
    auto lines = session_rows(6);
    lines.back() = "row-spin";
    text_ptr->set_lines(std::move(lines));
    const auto writes_before = terminal.output().size();
    REQUIRE(tui.render());
    CHECK_FALSE(terminal.check_clear_screen_called());
    CHECK(terminal.scrollback() == frozen_scrollback);
    const std::vector<std::string> spun_viewport{"row-0002", "row-0003", "row-0004", "row-spin"};
    CHECK(terminal.screen() == spun_viewport);

    // The frame is one synchronized differential write: begin, the changed
    // row with its line-flow move, end. Exactly one row is rewritten, and the
    // single erase covers only that row.
    const auto diff = std::span(terminal.output()).subspan(writes_before);
    REQUIRE(diff.size() == 3);
    CHECK(diff[0] == "\x1b[?2026h");
    CHECK(diff[1].find("row-spin") != std::string::npos);
    CHECK(diff[1].find("row-0004") == std::string::npos);
    CHECK(diff[1].find("\x1b[2K") != std::string::npos);
    CHECK(diff[1].find("\x1b[2K", diff[1].find("\x1b[2K") + 1) == std::string::npos);
    CHECK(diff[2] == "\x1b[?2026l");
    // The change sits on the last viewport row, so no scrolling was needed and
    // the cursor stays on that row.
    CHECK(diff[1].find("\r\n") == std::string::npos);
    CHECK(terminal.cursor().row == 3);
}

TEST_CASE("clearOnShrink clears the screen when content shrinks below the working area",
        "[tui][render][issue984][spec]") {
    cch::tui::VirtualTerminal terminal({.columns = 8, .rows = 4});
    cch::tui::Tui tui(terminal);
    tui.set_clear_on_shrink(true);

    auto text = std::make_unique<FixedTextComponent>(session_rows(6));
    auto* text_ptr = text.get();
    REQUIRE(tui.add_child(std::move(text)));
    REQUIRE(tui.start());
    REQUIRE(tui.render());
    (void)terminal.check_clear_screen_called();

    // Two lines are below the highest line count ever rendered (pi
    // clearOnShrink: newLines.length < maxLinesRendered), so pi re-renders
    // from a clean screen clearing viewport and scrollback together.
    text_ptr->set_lines({"row-0000", "row-0001"});
    REQUIRE(tui.render());
    CHECK(terminal.check_clear_screen_called());
    CHECK(terminal.check_clear_scrollback_called());

    const std::vector<std::string> expected_viewport{"row-0000", "row-0001", "", ""};
    CHECK(terminal.screen() == expected_viewport);
    CHECK(terminal.scrollback().empty());
    // fullRender recomputes the viewport top from the cleared buffer.
    CHECK(terminal.viewport_top() == 0);
    CHECK(terminal.cursor().row == 1);
}

TEST_CASE("An active overlay inhibits clearOnShrink and diffs the shrink in place", "[tui][render][issue984][spec]") {
    cch::tui::VirtualTerminal terminal({.columns = 5, .rows = 4});
    cch::tui::Tui tui(terminal);
    tui.set_clear_on_shrink(true);

    auto text = std::make_unique<FixedTextComponent>(std::vector<std::string>{"row-0", "row-1", "row-2", "row-3"});
    auto* text_ptr = text.get();
    REQUIRE(tui.add_child(std::move(text)));
    REQUIRE(tui.start());
    REQUIRE(tui.render());

    // Four 5-column rows fill the 4-row viewport exactly, so no history is
    // scrolled out.
    REQUIRE(terminal.viewport_top() == 0);

    cch::tui::OverlayOptions options;
    options.width = 1.0;
    options.col = 0.0;
    options.row = 0.0;
    auto overlay = std::make_unique<cch::tui::Overlay>(options);
    REQUIRE(overlay->add_child(std::make_unique<FixedTextComponent>(std::vector<std::string>{"*"})));
    REQUIRE(tui.add_overlay(std::move(overlay)));
    REQUIRE(tui.render());
    // The 1x1 overlay's `*` paints over the top-left cell of the visible
    // viewport; the base rows underneath are untouched.
    REQUIRE(terminal.screen().front() == "*ow-0");
    (void)terminal.check_clear_screen_called();

    // (pi clearOnShrink and !hasOverlayEntries): the overlay needs the padded
    // viewport, so the same shrink diffs in place instead of clearing.
    text_ptr->set_lines({"row-0", "row-1"});
    REQUIRE(tui.render());
    CHECK_FALSE(terminal.check_clear_screen_called());
    CHECK_FALSE(terminal.check_clear_scrollback_called());
    const std::vector<std::string> expected_viewport{"*ow-0", "row-1", "     ", "     "};
    CHECK(terminal.screen() == expected_viewport);
}

TEST_CASE("A rewrap after overflow reflows from a clean screen and re-anchors the viewport",
        "[tui][render][issue984][spec]") {
    cch::tui::VirtualTerminal terminal({.columns = 4, .rows = 3});
    cch::tui::Tui tui(terminal);

    // Nine 8-column rows wrap into 18 lines at 4 columns.
    REQUIRE(tui.add_child(std::make_unique<cch::tui::Text>(std::string(8 * 9, 'x'), 0, 0)));
    REQUIRE(tui.start());
    REQUIRE(tui.render());
    REQUIRE(terminal.scrollback().size() == 15);
    REQUIRE(terminal.viewport_top() == 15);

    // Widening to 8 columns undoes every wrap: nine rows remain, so pi still
    // clears and re-anchors the viewport at buffer row 6 (9 - 3 visible rows).
    REQUIRE(terminal.inject_resize({.columns = 8, .rows = 3}));
    (void)terminal.check_clear_screen_called();
    REQUIRE(tui.render());
    CHECK(terminal.check_clear_screen_called());
    CHECK(terminal.check_clear_scrollback_called());
    REQUIRE(terminal.screen().size() == 3);
    CHECK(terminal.viewport_top() == 6);
    CHECK(terminal.screen().back() == std::string(8, 'x'));

    // fullRender recomputes the top from the reflowed buffer instead of keeping
    // the pre-resize top of 15.
    CHECK(terminal.cursor().row == 2);
}

TEST_CASE(
        "A height shrink re-anchors the viewport top to the previous buffer length", "[tui][render][issue984][spec]") {
    cch::tui::VirtualTerminal terminal({.columns = 8, .rows = 5});
    cch::tui::Tui tui(terminal);

    REQUIRE(tui.add_child(std::make_unique<cch::tui::Text>(std::string(8 * 8, 'y'), 0, 0)));
    REQUIRE(tui.start());
    REQUIRE(tui.render());

    // Eight rows on a five-row viewport scrolled three into the history.
    REQUIRE(terminal.viewport_top() == 3);
    REQUIRE(terminal.inject_resize({.columns = 8, .rows = 3}));
    (void)terminal.check_clear_screen_called();

    // pi recomputes prevViewportTop from the previous buffer length (8 rows on
    // a now-3-row viewport) instead of reusing the stale top of 3.
    REQUIRE(tui.render());
    CHECK(terminal.check_clear_screen_called());
    CHECK(terminal.check_clear_scrollback_called());
    REQUIRE(terminal.screen().size() == 3);
    CHECK(terminal.viewport_top() == 5);
    CHECK(terminal.screen().back() == std::string(8, 'y'));
    CHECK(terminal.cursor().row == 2);
}

TEST_CASE(
        "Growing past the viewport scrolls with CRLF and re-anchors the buffer top", "[tui][render][issue984][spec]") {
    cch::tui::VirtualTerminal terminal({.columns = 6, .rows = 4});
    cch::tui::Tui tui(terminal);

    auto text = std::make_unique<FixedTextComponent>(std::vector<std::string>{"aaa", "bbb", "ccc", "ddd"});
    auto* text_ptr = text.get();
    REQUIRE(tui.add_child(std::move(text)));
    REQUIRE(tui.start());
    REQUIRE(tui.render());

    // Four rows exactly fill the viewport, so the buffer top is still row 0.
    REQUIRE(terminal.viewport_top() == 0);
    REQUIRE(terminal.scrollback().empty());

    // Two more rows push the buffer past the viewport: pi appends with a plain
    // CRLF scroll instead of a full redraw, so the scrolled-off rows land in
    // scrollback and the tracked buffer top advances by the scrolled amount.
    text_ptr->set_lines({"aaa", "bbb", "ccc", "ddd", "eee", "fff"});
    REQUIRE(tui.render());

    CHECK_FALSE(terminal.check_clear_screen_called());
    CHECK_FALSE(terminal.check_clear_scrollback_called());
    const std::vector<std::string> expected_scrollback{"aaa   ", "bbb   "};
    CHECK(terminal.scrollback() == expected_scrollback);
    CHECK(terminal.viewport_top() == 2);
    const std::vector<std::string> expected_screen{"ccc   ", "ddd   ", "eee   ", "fff   "};
    CHECK(terminal.screen() == expected_screen);
}

TEST_CASE("A one-row view change prepares only the changed composed row", "[tui][render][issue711][spec]") {
    cch::tui::VirtualTerminal terminal({.columns = 10, .rows = 8});
    cch::tui::Tui tui(terminal);

    auto text = std::make_unique<cch::tui::Text>("one\ntwo\nthree\nfour\nfive\nsix", 0, 0);
    auto* text_ptr = text.get();
    REQUIRE(tui.add_child(std::move(text)));
    REQUIRE(tui.start());
    REQUIRE(tui.render());
    CHECK(cch::tui::detail::testing::frame_prepare_call_count(tui) == 6);

    // Six composed rows are on screen; only the last one changes. Preparation
    // follows the change, not the transcript length.
    text_ptr->set_text("one\ntwo\nthree\nfour\nfive\nSIX");
    REQUIRE(tui.render());
    CHECK(cch::tui::detail::testing::frame_prepare_call_count(tui) == 1);
    CHECK(terminal.screen().front() == "one       ");
}

namespace {

class MockFaultyTerminal final : public cch::tui::Terminal {
public:
    explicit MockFaultyTerminal(cch::tui::VirtualTerminalOptions options = {}) : inner_(std::move(options)) {}

    cch::support::ExpectedVoid start(
            cch::tui::TerminalInputSink input_sink, cch::tui::TerminalResizeSink resize_sink) override {
        return inner_.start(std::move(input_sink), std::move(resize_sink));
    }
    cch::support::ExpectedVoid stop() override { return inner_.stop(); }
    cch::tui::TerminalDimensions dimensions() const override { return inner_.dimensions(); }
    cch::tui::TerminalCapabilities capabilities() const override { return inner_.capabilities(); }
    cch::tui::TerminalModeState modes() const override { return inner_.modes(); }

    cch::support::ExpectedVoid clear_screen() override {
        ++clear_count;
        return inner_.clear_screen();
    }

    cch::support::ExpectedVoid write(std::string_view output) override {
        ++write_count;
        if (write_count == fail_on_write_index) {
            return std::unexpected(cch::support::make_error(fail_code, "injected fault"));
        }
        written_chunks.emplace_back(output);
        return inner_.write(output);
    }

    cch::support::ExpectedVoid set_cursor(cch::tui::CursorPosition position) override {
        return inner_.set_cursor(position);
    }
    cch::support::ExpectedVoid set_cursor_visible(bool visible) override { return inner_.set_cursor_visible(visible); }
    cch::support::ExpectedVoid set_scroll_margins(std::size_t top_row, std::size_t bottom_row) override {
        return inner_.set_scroll_margins(top_row, bottom_row);
    }
    cch::support::ExpectedVoid reset_scroll_margins() override { return inner_.reset_scroll_margins(); }
    cch::support::ExpectedVoid begin_synchronized_update() override { return inner_.begin_synchronized_update(); }
    cch::support::ExpectedVoid end_synchronized_update() override { return inner_.end_synchronized_update(); }
    cch::support::ExpectedVoid set_title(std::string_view title) override { return inner_.set_title(title); }
    cch::support::ExpectedVoid apply_cell_pixel_dimensions(cch::tui::CellPixelDimensions pixels) override {
        return inner_.apply_cell_pixel_dimensions(pixels);
    }
    cch::support::ExpectedVoid set_progress(bool active) override { return inner_.set_progress(active); }
    cch::support::ExpectedVoid drain_input(std::chrono::milliseconds max_ms = cch::tui::kDrainInputMaxMs,
            std::chrono::milliseconds idle_ms = cch::tui::kDrainInputIdleMs) override {
        return inner_.drain_input(max_ms, idle_ms);
    }

    std::size_t clear_count{0};
    std::size_t write_count{0};
    std::size_t fail_on_write_index{std::numeric_limits<std::size_t>::max()};
    cch::support::ErrorCode fail_code{cch::support::ErrorCode::Busy};
    std::vector<std::string> written_chunks;

private:
    cch::tui::VirtualTerminal inner_;
};

} // namespace

TEST_CASE("RenderPipeline tracks admitted frame prefix and exact-once frame accounting under Busy",
        "[tui][render][issue986][spec]") {
    MockFaultyTerminal terminal;
    REQUIRE(terminal.start([](std::string) -> cch::support::ExpectedVoid { return {}; },
            [](cch::tui::TerminalDimensions) -> cch::support::ExpectedVoid { return {}; }));
    cch::tui::detail::RenderPipeline pipeline(terminal);
    const cch::tui::TerminalDimensions dims{.columns = 40, .rows = 10};
    pipeline.start(dims);

    CHECK(pipeline.admitted_prefix(dims) == 0);

    // In full_render with 4 lines, each line is a separate write:
    // write 1: line 0
    // write 2: line 1
    // write 3: line 2 -> injected fault
    terminal.fail_on_write_index = 3;
    terminal.fail_code = cch::support::ErrorCode::Busy;

    cch::tui::RenderResult frame1;
    frame1.lines = {"alpha", "beta", "gamma", "delta"};
    const cch::tui::TerminalCapabilities caps{.synchronized_output = false};
    auto res1 = pipeline.render(std::move(frame1), dims, caps, false, false, false, std::nullopt);
    REQUIRE_FALSE(res1);
    CHECK(res1.error().code == cch::support::ErrorCode::Busy);

    // Lines 0 and 1 were admitted before the write failed at line 2.
    CHECK(pipeline.admitted_prefix(dims) == 2);
    // On resize, admitted prefix resets to 0.
    CHECK(pipeline.admitted_prefix({.columns = 50, .rows = 10}) == 0);

    // Retry the frame on the same dimensions without fault.
    terminal.fail_on_write_index = std::numeric_limits<std::size_t>::max();
    terminal.written_chunks.clear();

    cch::tui::RenderResult frame2;
    frame2.lines = {"alpha", "beta", "gamma", "delta"};
    auto res2 = pipeline.render(std::move(frame2), dims, caps, false, false, false, std::nullopt);
    REQUIRE(res2);

    // Exact-once accounting: alpha and beta were not re-emitted; gamma and delta were written.
    CHECK(pipeline.admitted_prefix(dims) == 4);
    REQUIRE(terminal.written_chunks.size() == 1);
    CHECK(terminal.written_chunks[0].find("alpha") == std::string::npos);
    CHECK(terminal.written_chunks[0].find("beta") == std::string::npos);
    CHECK(terminal.written_chunks[0].find("gamma") != std::string::npos);
    CHECK(terminal.written_chunks[0].find("delta") != std::string::npos);
}

TEST_CASE("RenderPipeline rollback on non-busy failure preserves admitted state", "[tui][render][issue986][spec]") {
    MockFaultyTerminal terminal;
    REQUIRE(terminal.start([](std::string) -> cch::support::ExpectedVoid { return {}; },
            [](cch::tui::TerminalDimensions) -> cch::support::ExpectedVoid { return {}; }));
    cch::tui::detail::RenderPipeline pipeline(terminal);
    const cch::tui::TerminalDimensions dims{.columns = 40, .rows = 10};
    pipeline.start(dims);

    // Inject non-busy Process error on first render.
    terminal.fail_on_write_index = 2;
    terminal.fail_code = cch::support::ErrorCode::Process;

    cch::tui::RenderResult frame;
    frame.lines = {"one", "two", "three"};
    const cch::tui::TerminalCapabilities caps{.synchronized_output = false};
    auto res = pipeline.render(std::move(frame), dims, caps, false, false, false, std::nullopt);
    REQUIRE_FALSE(res);
    CHECK(res.error().code == cch::support::ErrorCode::Process);

    // Non-busy error rolls back: nothing is admitted.
    CHECK(pipeline.admitted_prefix(dims) == 0);
}

TEST_CASE("RenderPipeline cleared screen frame recovery under Busy", "[tui][render][issue986][spec]") {
    MockFaultyTerminal terminal;
    REQUIRE(terminal.start([](std::string) -> cch::support::ExpectedVoid { return {}; },
            [](cch::tui::TerminalDimensions) -> cch::support::ExpectedVoid { return {}; }));
    cch::tui::detail::RenderPipeline pipeline(terminal);
    const cch::tui::TerminalDimensions dims{.columns = 40, .rows = 10};
    pipeline.start(dims);

    // Initial render of 4 lines succeeds.
    cch::tui::RenderResult initial_frame;
    initial_frame.lines = {"A", "B", "C", "D"};
    const cch::tui::TerminalCapabilities caps{.synchronized_output = false};
    REQUIRE(pipeline.render(std::move(initial_frame), dims, caps, false, false, false, std::nullopt));
    CHECK(pipeline.admitted_prefix(dims) == 4);

    const auto clear_count_before = terminal.clear_count;

    // Shrinking from 4 to 2 lines with clear_on_shrink=true triggers a clear_screen.
    // In full_render(true), write 1 is line 0 ("X"), write 2 is line 1 ("Y").
    // Inject Busy fault on write 2 (line 1).
    terminal.fail_on_write_index = terminal.write_count + 2;
    terminal.fail_code = cch::support::ErrorCode::Busy;

    cch::tui::RenderResult shrink_frame1;
    shrink_frame1.lines = {"X", "Y"};
    auto res_shrink1 = pipeline.render(std::move(shrink_frame1), dims, caps, false, false, true, std::nullopt);
    REQUIRE_FALSE(res_shrink1);
    CHECK(res_shrink1.error().code == cch::support::ErrorCode::Busy);
    CHECK(terminal.clear_count == clear_count_before + 1);
    // Line 0 was admitted on the cleared screen.
    CHECK(pipeline.admitted_prefix(dims) == 1);

    // Retry the shrink frame: resumable clear must NOT clear the screen a second time.
    terminal.fail_on_write_index = std::numeric_limits<std::size_t>::max();
    terminal.written_chunks.clear();

    cch::tui::RenderResult shrink_frame2;
    shrink_frame2.lines = {"X", "Y"};
    auto res_shrink2 = pipeline.render(std::move(shrink_frame2), dims, caps, false, false, true, std::nullopt);
    REQUIRE(res_shrink2);
    // Screen was NOT cleared again!
    CHECK(terminal.clear_count == clear_count_before + 1);
    CHECK(pipeline.admitted_prefix(dims) == 2);
    // Only line 1 ("Y") was written on retry.
    REQUIRE(terminal.written_chunks.size() == 1);
    CHECK(terminal.written_chunks[0].find("Y") != std::string::npos);
}

TEST_CASE("Main-screen accounting separates attempted from committed frame writes under Busy",
        "[tui][render][diagnostics][issue1016][spec]") {
    MockFaultyTerminal terminal(cch::tui::VirtualTerminalOptions{.columns = 4, .rows = 3});
    cch::tui::Tui tui(terminal);
    auto text = std::make_unique<cch::tui::Text>("ab\ncd", 0, 0);
    REQUIRE(tui.add_child(std::move(text)));
    REQUIRE(tui.start());

    CHECK(tui.full_redraws() == 0);
    CHECK(tui.write_accounting() == cch::tui::TuiWriteAccounting{});

    // Frame 1: the first render is one full-redraw attempt written row by row,
    // and every row is admitted.
    REQUIRE(tui.render());
    CHECK(tui.full_redraws() == 1);
    REQUIRE(terminal.written_chunks.size() == 2);
    CHECK(tui.write_accounting().attempted_writes == 2);
    CHECK(tui.write_accounting().committed_writes == 2);
    CHECK(tui.write_accounting().committed_bytes ==
            terminal.written_chunks[0].size() + terminal.written_chunks[1].size());

    // Frame 2: the cleared baseline forces another full redraw, and its first
    // row write is refused with Busy. The attempt is counted, nothing commits.
    REQUIRE(tui.clear_screen());
    terminal.fail_on_write_index = terminal.write_count + 1;
    terminal.fail_code = cch::support::ErrorCode::Busy;
    const auto refused = tui.render();
    REQUIRE_FALSE(refused);
    CHECK(refused.error().code == cch::support::ErrorCode::Busy);
    CHECK(tui.full_redraws() == 2);
    CHECK(tui.write_accounting().attempted_writes == 1);
    CHECK(tui.write_accounting().committed_writes == 0);
    CHECK(tui.write_accounting().committed_bytes == 0);
    REQUIRE(terminal.written_chunks.size() == 2);

    // Frame 3: the retry is its own attempt and commits both rows exactly once.
    terminal.fail_on_write_index = std::numeric_limits<std::size_t>::max();
    REQUIRE(tui.render());
    CHECK(tui.full_redraws() == 3);
    REQUIRE(terminal.written_chunks.size() == 4);
    CHECK(tui.write_accounting().attempted_writes == 2);
    CHECK(tui.write_accounting().committed_writes == 2);
    CHECK(tui.write_accounting().committed_bytes ==
            terminal.written_chunks[2].size() + terminal.written_chunks[3].size());
    CHECK(terminal.written_chunks[2].find("ab") != std::string::npos);
    CHECK(terminal.written_chunks[3].find("cd") != std::string::npos);
}

TEST_CASE("Main-screen accounting windows per frame and keeps the redraw count across a restart",
        "[tui][render][diagnostics][issue1016][spec]") {
    cch::tui::VirtualTerminal terminal({.columns = 4, .rows = 3});
    cch::tui::Tui tui(terminal);
    REQUIRE(tui.add_child(std::make_unique<cch::tui::Text>("ab\ncd", 0, 0)));
    REQUIRE(tui.start());
    REQUIRE(tui.render());
    REQUIRE(tui.full_redraws() == 1);
    const auto first_frame = tui.write_accounting();
    CHECK(first_frame.attempted_writes == 2);

    REQUIRE(tui.stop());
    REQUIRE(tui.start());

    // pi's `fullRedrawCount` is a lifetime total: start() does not reset it.
    CHECK(tui.full_redraws() == 1);

    REQUIRE(tui.render());
    CHECK(tui.full_redraws() == 2);
    // The accounting window is the last frame, not an accumulated total: the
    // restarted frame commits the same two rows once.
    CHECK(tui.write_accounting() == first_frame);
    CHECK(tui.write_accounting().attempted_writes == 2);
    CHECK(tui.write_accounting().committed_writes == 2);

    // A frame that changes nothing attempts no redraw and commits no write.
    REQUIRE(tui.render());
    CHECK(tui.full_redraws() == 2);
    CHECK(tui.write_accounting() == cch::tui::TuiWriteAccounting{});

    REQUIRE(tui.stop());
}

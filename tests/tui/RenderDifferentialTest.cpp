#include <cch/tui/Text.hpp>
#include <cch/tui/Tui.hpp>
#include <cch/tui/VirtualTerminal.hpp>

#include <cch/util/Error.hpp>
#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>
#include <vector>

namespace {

/// A Component that returns a fixed set of lines on each render.
class FixedTextComponent final : public cch::tui::Component {
public:
    explicit FixedTextComponent(std::vector<std::string> lines)
        : lines_(std::move(lines)) {}

    void set_lines(std::vector<std::string> lines) {
        lines_ = std::move(lines);
        cache_valid_ = false;
    }

    [[nodiscard]] cch::util::Expected<cch::tui::RenderResult> render(std::size_t) override {
        if (cache_valid_) return cch::tui::RenderResult{.lines = cached_};
        cached_ = lines_;
        cache_valid_ = true;
        return cch::tui::RenderResult{.lines = lines_};
    }

    void invalidate() override {
        cache_valid_ = false;
    }

private:
    std::vector<std::string> lines_;
    std::vector<std::string> cached_;
    bool cache_valid_{false};
};

/// A focusable that reports a fixed buffer-relative cursor row and renders no
/// lines, for pinning the IME cursor viewport clamp over scrolled content.
class CursorReportingComponent final
    : public cch::tui::Component,
      public cch::tui::Focusable {
public:
    explicit CursorReportingComponent(std::size_t cursor_row)
        : cursor_row_(cursor_row) {}

    void set_row(std::size_t row) {
        cursor_row_ = row;
    }

    [[nodiscard]] cch::util::Expected<cch::tui::RenderResult> render(std::size_t) override {
        return cch::tui::RenderResult{};
    }

    void invalidate() override {}

    void set_focused(bool focused) override {
        focused_ = focused;
    }

    [[nodiscard]] bool focused() const override {
        return focused_;
    }

    [[nodiscard]] std::optional<cch::tui::CursorPosition> cursor_location() const override {
        return cch::tui::CursorPosition{.column = 0, .row = cursor_row_};
    }

private:
    std::size_t cursor_row_;
    bool focused_{false};
};

} // namespace

TEST_CASE("First render writes visible content without clearing scrollback", "[tui][render][issue49]") {
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

TEST_CASE("Normal updates begin at first changed visible line", "[tui][render][issue49]") {
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

TEST_CASE("Shrinking content clears stale rows below new content", "[tui][render][issue49]") {
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

TEST_CASE("Width change triggers full redraw with clear screen", "[tui][render][issue49]") {
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

TEST_CASE("Resize full redraw emits the pi-exact clear-screen and scrollback bytes", "[tui][render][issue49][issue435]") {
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

TEST_CASE("Tui clamps the IME cursor to the visible viewport over scrolled content", "[tui][render][issue49][issue435]") {
    cch::tui::VirtualTerminal terminal({.columns = 8, .rows = 3});
    cch::tui::Tui tui(terminal);
    REQUIRE(tui.add_child(std::make_unique<cch::tui::Text>("one\ntwo\nthree\nfour\nfive", 0, 0)));
    auto cursor = std::make_unique<CursorReportingComponent>(4);
    auto* cursor_ptr = cursor.get();
    REQUIRE(tui.add_child(std::move(cursor)));
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

TEST_CASE("Changes at the viewport top diff in place without clearing", "[tui][render][issue49][issue435]") {
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

TEST_CASE("Changes above the tracked viewport trigger a full redraw", "[tui][render][issue49][issue435]") {
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

TEST_CASE("Supported synchronized output wraps a render atomically", "[tui][render][issue49]") {
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

TEST_CASE("Render requests coalesce until render and include resize", "[tui][render][issue58]") {
    cch::tui::VirtualTerminal terminal({.columns = 7, .rows = 2});
    cch::tui::Tui tui(terminal);
    REQUIRE(tui.add_child(std::make_unique<cch::tui::Text>("initial", 0, 0)));
    std::size_t requests = 0;
    tui.set_render_request_sink([&requests] { ++requests; });
    REQUIRE(tui.start());
    REQUIRE(tui.render());

    tui.invalidate();
    tui.invalidate();
    CHECK(requests == 1);
    REQUIRE(tui.render());

    tui.invalidate();
    CHECK(requests == 2);
    REQUIRE(tui.render());
    REQUIRE(terminal.inject_resize({.columns = 8, .rows = 2}));
    CHECK(requests == 3);
}

TEST_CASE("Repeated invalidate coalesces without losing latest state", "[tui][render][issue49]") {
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

TEST_CASE("Empty content renders zero lines correctly", "[tui][render][issue49]") {
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

TEST_CASE("Shrink to empty content clears stale rows", "[tui][render][issue49]") {
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

TEST_CASE("Viewport height change triggers a full redraw with clear", "[tui][render][issue49][issue435]") {
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

TEST_CASE("Pending render request is consumed on render call", "[tui][render][issue49]") {
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

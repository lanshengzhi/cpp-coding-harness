#include <cch/tui/Text.hpp>
#include <cch/tui/Tui.hpp>
#include <cch/tui/VirtualTerminal.hpp>

#include <cch/support/Error.hpp>
#include "tui/TuiTestHooks.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <charconv>
#include <format>
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

    [[nodiscard]] cch::support::Expected<cch::tui::RenderResult> render(std::size_t) override {
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

    [[nodiscard]] cch::support::Expected<cch::tui::RenderResult> render(std::size_t) override {
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

/// A Component whose transcript and dock can change between frames: the shape
/// of a long session whose editor line changes on a keystroke.
class TranscriptDockComponent final : public cch::tui::Component {
public:
    void set_lines(std::vector<std::string> lines) { lines_ = std::move(lines); }
    void set_dock_lines(std::vector<std::string> dock_lines) { dock_lines_ = std::move(dock_lines); }
    void set_viewport_height(std::size_t height) {
        viewport_height_ = height;
        has_viewport_height_ = true;
    }

    [[nodiscard]] cch::support::Expected<cch::tui::RenderResult> render(std::size_t) override {
        cch::tui::RenderResult result;
        result.lines = lines_;
        result.dock_lines = dock_lines_;
        if (has_viewport_height_) result.viewport_height = viewport_height_;
        return result;
    }

    void invalidate() override {}

private:
    std::vector<std::string> lines_;
    std::vector<std::string> dock_lines_;
    std::size_t viewport_height_{0};
    bool has_viewport_height_{false};
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
/// across refusals. Admitted buffer rows and emitted bytes are recorded per
/// frame.
class BoundedQueueTerminal final : public cch::tui::Terminal {
public:
    BoundedQueueTerminal(cch::tui::TerminalDimensions dimensions, std::size_t capacity)
        : dimensions_(dimensions), capacity_(capacity) {}

    void drain() { queued_ = 0; }
    [[nodiscard]] std::size_t emitted_bytes() const { return emitted_bytes_; }
    [[nodiscard]] const std::vector<std::size_t>& cursor_rows() const { return cursor_rows_; }
    [[nodiscard]] std::size_t dock_cursor_calls() const { return dock_cursor_calls_; }
    [[nodiscard]] std::size_t blank_row_writes() const { return blank_row_writes_; }
    [[nodiscard]] std::size_t clear_screen_calls() const { return clear_screen_calls_; }
    [[nodiscard]] std::size_t last_written_row() const { return last_written_row_; }
    void reset_frame_counters() {
        emitted_bytes_ = 0;
        cursor_rows_.clear();
        dock_cursor_calls_ = 0;
        blank_row_writes_ = 0;
        last_written_row_ = 0;
    }

    [[nodiscard]] cch::support::ExpectedVoid start(
            cch::tui::TerminalInputSink, cch::tui::TerminalResizeSink resize_sink) override {
        resize_sink_ = std::move(resize_sink);
        modes_.started = true;
        return {};
    }
    [[nodiscard]] cch::support::ExpectedVoid stop() override {
        modes_.started = false;
        return {};
    }
    [[nodiscard]] cch::support::ExpectedVoid inject_resize(cch::tui::TerminalDimensions dimensions) {
        dimensions_ = dimensions;
        if (!resize_sink_) return {};
        return resize_sink_(dimensions);
    }
    [[nodiscard]] cch::tui::TerminalDimensions dimensions() const override { return dimensions_; }
    // No synchronized output: each escape sequence is delivered on its own, so
    // a refused write leaves the frame partially admitted, as under tmux.
    [[nodiscard]] cch::tui::TerminalCapabilities capabilities() const override { return {}; }
    [[nodiscard]] cch::tui::TerminalModeState modes() const override { return modes_; }
    [[nodiscard]] cch::support::ExpectedVoid clear_screen() override {
        if (!admit(kEscapeCost)) return busy();
        ++clear_screen_calls_;
        return {};
    }
    [[nodiscard]] cch::support::ExpectedVoid write(std::string_view output) override {
        if (!admit(output.size())) return busy();
        if (!output.empty() && output.find_first_not_of(' ') == std::string_view::npos) {
            ++blank_row_writes_;
        } else if (output.starts_with("row-")) {
            // Composed transcript rows carry their index, so a test can tell
            // which rows the terminal has actually received.
            std::size_t row = 0;
            const auto digits = output.substr(4, 4);
            const auto [_, parse_error] = std::from_chars(digits.data(), digits.data() + digits.size(), row);
            if (parse_error == std::errc{}) last_written_row_ = row;
        }
        return {};
    }
    [[nodiscard]] cch::support::ExpectedVoid set_cursor(cch::tui::CursorPosition position) override {
        if (!admit(kEscapeCost)) return busy();
        cursor_rows_.push_back(position.row);
        return {};
    }
    [[nodiscard]] cch::support::ExpectedVoid set_cursor_visible(bool) override { return {}; }
    [[nodiscard]] cch::support::ExpectedVoid set_scroll_margins(std::size_t, std::size_t) override {
        if (!admit(kEscapeCost)) return busy();
        return {};
    }
    [[nodiscard]] cch::support::ExpectedVoid reset_scroll_margins() override {
        if (!admit(kEscapeCost)) return busy();
        return {};
    }
    [[nodiscard]] cch::support::ExpectedVoid set_dock_cursor(std::size_t, std::size_t) override {
        if (!admit(kEscapeCost)) return busy();
        ++dock_cursor_calls_;
        return {};
    }
    [[nodiscard]] cch::support::Expected<cch::tui::TerminalImageHandle> place_image(
            const cch::tui::TerminalImage&) override {
        return cch::tui::TerminalImageHandle{};
    }
    [[nodiscard]] cch::support::ExpectedVoid remove_image(
            cch::tui::TerminalImageHandle, const cch::tui::CellRegion&) override {
        return {};
    }
    [[nodiscard]] cch::support::ExpectedVoid begin_synchronized_update() override { return {}; }
    [[nodiscard]] cch::support::ExpectedVoid end_synchronized_update() override { return {}; }
    [[nodiscard]] cch::support::ExpectedVoid set_title(std::string_view) override { return {}; }
    [[nodiscard]] cch::support::ExpectedVoid set_progress(bool) override { return {}; }
    [[nodiscard]] cch::support::ExpectedVoid drain_input(
            std::chrono::milliseconds, std::chrono::milliseconds) override {
        return {};
    }

private:
    static constexpr std::size_t kEscapeCost = 8;

    [[nodiscard]] static cch::support::ExpectedVoid busy() {
        return std::unexpected(
                cch::support::make_error(cch::support::ErrorCode::Busy, "bounded queue cannot admit more output"));
    }
    [[nodiscard]] bool admit(std::size_t bytes) {
        if (queued_ + bytes > capacity_) return false;
        queued_ += bytes;
        emitted_bytes_ += bytes;
        return true;
    }

    cch::tui::TerminalDimensions dimensions_;
    std::size_t capacity_{0};
    std::size_t queued_{0};
    std::size_t emitted_bytes_{0};
    std::vector<std::size_t> cursor_rows_;
    std::size_t dock_cursor_calls_{0};
    std::size_t blank_row_writes_{0};
    std::size_t clear_screen_calls_{0};
    std::size_t last_written_row_{0};
    cch::tui::TerminalResizeSink resize_sink_;
    cch::tui::TerminalModeState modes_;
};

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
    cch::tui::Tui tui(terminal);
    REQUIRE(tui.add_child(std::make_unique<cch::tui::Text>("initial", 0, 0)));
    std::size_t requests = 0;
    tui.set_render_request_sink([&requests]() -> cch::support::ExpectedVoid { ++requests; return {}; });
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

TEST_CASE("Tui resumes a backpressured frame after its admitted rows", "[tui][render][issue732][spec]") {
    constexpr std::size_t kRows = 60;
    constexpr std::size_t kCapacity = 400;
    BoundedQueueTerminal terminal({.columns = 12, .rows = 4}, kCapacity);
    cch::tui::Tui tui(terminal);
    auto component = std::make_unique<TranscriptDockComponent>();
    auto* view = component.get();
    REQUIRE(tui.add_child(std::move(component)));
    REQUIRE(tui.start());

    // A committed one-row frame, then a much larger one. The large frame is
    // refused mid-buffer, so the terminal holds rows [0, admitted) and the
    // committed buffer holds all kRows.
    view->set_lines(session_rows(1));
    REQUIRE(tui.render());
    terminal.drain();
    terminal.reset_frame_counters();

    view->set_lines(session_rows(kRows));
    // Every refusal leaves the rows the terminal admitted behind it, and every
    // retry continues at the next row instead of re-emitting the buffer from
    // its first changed row (row 1, after the committed one-row frame).
    std::size_t expected_next = 1;
    for (std::size_t attempt = 0; attempt < 50; ++attempt) {
        const auto rendered = tui.render();
        REQUIRE_FALSE(terminal.cursor_rows().empty());
        CHECK(terminal.cursor_rows().front() == expected_next);
        expected_next = terminal.last_written_row() + 1;
        if (rendered) break;
        CHECK(rendered.error().code == cch::support::ErrorCode::Busy);
        terminal.drain();
        terminal.reset_frame_counters();
    }
    CHECK(expected_next == kRows);
}

TEST_CASE("A dock-only frame after a backpressured paint emits only the dock", "[tui][render][issue732][spec]") {
    constexpr std::size_t kRows = 2000;
    constexpr std::size_t kWidth = 80;
    constexpr std::size_t kCapacity = 16 * 1024;
    BoundedQueueTerminal terminal({.columns = kWidth, .rows = 4}, kCapacity);
    cch::tui::Tui tui(terminal);
    auto component = std::make_unique<TranscriptDockComponent>();
    auto* view = component.get();
    REQUIRE(tui.add_child(std::move(component)));
    REQUIRE(tui.start());

    view->set_lines(session_rows(kRows));
    view->set_dock_lines({"editor"});
    view->set_viewport_height(2);

    // The composed buffer is far larger than one admission, so the paint is
    // refused repeatedly and completes only by resuming after each refusal.
    // Repeatedly re-emitting rows [0, admitted) instead consumes every drain
    // and never reaches the dock.
    bool painted = false;
    for (std::size_t attempt = 0; attempt < 100 && !painted; ++attempt) {
        painted = static_cast<bool>(tui.render());
        terminal.drain();
    }
    REQUIRE(painted);

    // A keystroke changes only the editor line. The frame must emit the dock
    // row, never the transcript (`kRows * kWidth` composed bytes).
    terminal.drain();
    terminal.reset_frame_counters();
    view->set_dock_lines({"editor!"});
    REQUIRE(tui.render());
    CHECK(terminal.cursor_rows().empty());
    CHECK(terminal.dock_cursor_calls() >= 1);
    CHECK(terminal.emitted_bytes() >= kWidth);
    CHECK(terminal.emitted_bytes() <= 2 * kWidth);
}

TEST_CASE("Tui resumes a backpressured clear repaint after its admitted rows", "[tui][render][issue732][spec]") {
    constexpr std::size_t kRows = 40;
    // Admits the margins, the clear, and a few rows before refusing the rest.
    BoundedQueueTerminal terminal({.columns = 12, .rows = 4}, 100);
    cch::tui::Tui tui(terminal);
    auto component = std::make_unique<TranscriptDockComponent>();
    auto* view = component.get();
    REQUIRE(tui.add_child(std::move(component)));
    REQUIRE(tui.start());

    // Paint 40 rows on a 4-row screen, leaving the viewport scrolled to row 36.
    view->set_lines(session_rows(kRows));
    bool painted = false;
    for (std::size_t attempt = 0; attempt < 50 && !painted; ++attempt) {
        painted = static_cast<bool>(tui.render());
        terminal.drain();
    }
    REQUIRE(painted);

    // Shrinking to 5 rows ends above the scrolled viewport, so the frame must
    // clear and reflow. The refusal must resume at the admitted row instead of
    // clearing and re-emitting from row zero on every retry.
    terminal.drain();
    terminal.reset_frame_counters();
    view->set_lines(session_rows(5));
    std::size_t expected_next = 0;
    bool finished = false;
    for (std::size_t attempt = 0; attempt < 50 && !finished; ++attempt) {
        const auto rendered = tui.render();
        REQUIRE_FALSE(terminal.cursor_rows().empty());
        CHECK(terminal.cursor_rows().front() == expected_next);
        expected_next = terminal.last_written_row() + 1;
        finished = static_cast<bool>(rendered);
        if (!rendered) {
            CHECK(rendered.error().code == cch::support::ErrorCode::Busy);
            terminal.drain();
            terminal.reset_frame_counters();
        }
    }
    REQUIRE(finished);
    CHECK(expected_next == 5);
}

TEST_CASE("Tui repaints a cleared dock when the dock write is refused", "[tui][render][issue732][spec]") {
    constexpr std::size_t kRows = 40;
    // Admits the margins, the clear, both viewport rows, and the dock cursor,
    // but not the dock row itself.
    BoundedQueueTerminal terminal({.columns = 12, .rows = 4}, 100);
    cch::tui::Tui tui(terminal);
    auto component = std::make_unique<TranscriptDockComponent>();
    auto* view = component.get();
    REQUIRE(tui.add_child(std::move(component)));
    REQUIRE(tui.start());

    view->set_lines(session_rows(kRows));
    view->set_dock_lines({"editor"});
    view->set_viewport_height(3);
    bool painted = false;
    for (std::size_t attempt = 0; attempt < 50 && !painted; ++attempt) {
        painted = static_cast<bool>(tui.render());
        terminal.drain();
    }
    REQUIRE(painted);

    // Shrinking above the viewport clears the screen, so the previously drawn
    // dock is erased. The viewport rows fit and the dock write is refused.
    terminal.drain();
    terminal.reset_frame_counters();
    view->set_lines(session_rows(2));
    const auto shrunk = tui.render();
    REQUIRE_FALSE(shrunk);
    CHECK(shrunk.error().code == cch::support::ErrorCode::Busy);
    CHECK(terminal.dock_cursor_calls() >= 1);

    // The dock content did not change, but the retry still owes the repaint the
    // clear erased: skipping it would leave the dock blank.
    terminal.drain();
    terminal.reset_frame_counters();
    REQUIRE(tui.render());
    CHECK(terminal.dock_cursor_calls() >= 1);
    CHECK(terminal.emitted_bytes() >= 6);
}

TEST_CASE("Tui clears again when a pending clear repaint changes above the visible top",
        "[tui][render][issue732][spec]") {
    constexpr std::size_t kRows = 60;
    // Admits the margins, the clear, and five 10-column rows, but not the sixth.
    BoundedQueueTerminal terminal({.columns = 12, .rows = 4}, 170);
    cch::tui::Tui tui(terminal);
    auto component = std::make_unique<TranscriptDockComponent>();
    auto* view = component.get();
    REQUIRE(tui.add_child(std::move(component)));
    REQUIRE(tui.start());

    view->set_lines(session_rows(kRows));
    bool painted = false;
    for (std::size_t attempt = 0; attempt < 50 && !painted; ++attempt) {
        painted = static_cast<bool>(tui.render());
        terminal.drain();
    }
    REQUIRE(painted);

    // A width change reflows from a clean screen; the repaint is refused with
    // its visible top above row zero.
    terminal.drain();
    REQUIRE(terminal.inject_resize({.columns = 10, .rows = 4}));
    const auto resized = tui.render();
    REQUIRE_FALSE(resized);
    CHECK(resized.error().code == cch::support::ErrorCode::Busy);
    const auto clears_after_resize = terminal.clear_screen_calls();
    REQUIRE(clears_after_resize >= 1);

    // A change above the pending repaint's visible top has scrolled away and
    // cannot be reached in place: the retry must clear again.
    auto changed = session_rows(kRows);
    changed.front() = "changed!";
    view->set_lines(std::move(changed));
    terminal.drain();
    const auto changed_frame = tui.render();
    REQUIRE_FALSE(changed_frame);
    CHECK(changed_frame.error().code == cch::support::ErrorCode::Busy);
    CHECK(terminal.clear_screen_calls() == clears_after_resize + 1);

    // The new repaint then converges by resuming its own admitted prefix.
    bool finished = false;
    for (std::size_t attempt = 0; attempt < 50 && !finished; ++attempt) {
        terminal.drain();
        finished = static_cast<bool>(tui.render());
    }
    REQUIRE(finished);
}

TEST_CASE("Tui finishes a backpressured clear-on-shrink tail clear", "[tui][render][issue732][spec]") {
    constexpr std::size_t kRows = 30;
    constexpr std::size_t kShrunkRows = 10;
    // Small enough that the tail clear is refused partway, large enough to
    // admit the frame's margins and a couple of cleared rows per attempt.
    BoundedQueueTerminal terminal({.columns = 12, .rows = 40}, 48);
    cch::tui::Tui tui(terminal);
    auto component = std::make_unique<TranscriptDockComponent>();
    auto* view = component.get();
    REQUIRE(tui.add_child(std::move(component)));
    REQUIRE(tui.start());

    view->set_lines(session_rows(kRows));
    bool painted = false;
    for (std::size_t attempt = 0; attempt < 50 && !painted; ++attempt) {
        painted = static_cast<bool>(tui.render());
        terminal.drain();
    }
    REQUIRE(painted);

    // The shrink stays inside the unscrolled viewport, so the frame takes the
    // differential route and owes a clear of rows [10, 30). A refusal in the
    // middle of that clear must not let the retry skip the rest of the tail.
    terminal.drain();
    terminal.reset_frame_counters();
    view->set_lines(session_rows(kShrunkRows));
    std::size_t cleared_rows = 0;
    bool finished = false;
    for (std::size_t attempt = 0; attempt < 50 && !finished; ++attempt) {
        terminal.drain();
        terminal.reset_frame_counters();
        const auto rendered = tui.render();
        cleared_rows += terminal.blank_row_writes();
        finished = static_cast<bool>(rendered);
        if (!rendered) CHECK(rendered.error().code == cch::support::ErrorCode::Busy);
    }
    REQUIRE(finished);
    CHECK(cleared_rows >= kRows - kShrunkRows);
}

TEST_CASE("Tui forces a clean repaint when content above a pending clear changes", "[tui][render][issue732][spec]") {
    constexpr std::size_t kRows = 40;
    constexpr std::size_t kShrunkRows = 20;
    // Small enough to interrupt the clear repaint after a few rows, large
    // enough to admit the frame's margins and keep the visible top above row 0.
    BoundedQueueTerminal terminal({.columns = 12, .rows = 4}, 200);
    cch::tui::Tui tui(terminal);
    auto component = std::make_unique<TranscriptDockComponent>();
    auto* view = component.get();
    REQUIRE(tui.add_child(std::move(component)));
    REQUIRE(tui.start());

    // Scrolled buffer: 40 rows on a 4-row screen leaves the viewport top at 36.
    view->set_lines(session_rows(kRows));
    bool painted = false;
    for (std::size_t attempt = 0; attempt < 50 && !painted; ++attempt) {
        painted = static_cast<bool>(tui.render());
        terminal.drain();
    }
    REQUIRE(painted);

    // The shrink ends above the viewport, so the frame clears and reflows from
    // the screen top. It is refused partway, leaving a pending clear whose
    // admitted prefix still scrolls the visible top past row 0.
    terminal.drain();
    view->set_lines(session_rows(kShrunkRows));
    const auto shrunk = tui.render();
    REQUIRE_FALSE(shrunk);
    CHECK(shrunk.error().code == cch::support::ErrorCode::Busy);
    const auto clears_after_shrink = terminal.clear_screen_calls();
    REQUIRE(clears_after_shrink >= 1);

    // A change above the pending clear's visible top cannot be reached in
    // place: the retry must clear again rather than resume over scrolled rows.
    auto changed = session_rows(kShrunkRows);
    changed.front() = "changed-row";
    view->set_lines(std::move(changed));
    const auto changed_frame = tui.render();
    REQUIRE_FALSE(changed_frame);
    CHECK(changed_frame.error().code == cch::support::ErrorCode::Busy);
    CHECK(terminal.clear_screen_calls() == clears_after_shrink + 1);

    // The repaint then converges by resuming its own admitted prefix.
    bool finished = false;
    for (std::size_t attempt = 0; attempt < 50 && !finished; ++attempt) {
        terminal.drain();
        const auto rendered = tui.render();
        finished = static_cast<bool>(rendered);
    }
    REQUIRE(finished);
}

#include <cch/tui/AltScreen.hpp>
#include <cch/tui/Text.hpp>
#include <cch/tui/VirtualTerminal.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

class AltChild final : public cch::tui::Component, public cch::tui::Focusable, public cch::tui::InputHandler {
public:
    explicit AltChild(std::vector<std::string> lines) : lines_(std::move(lines)) {}

    void set_lines(std::vector<std::string> lines) { lines_ = std::move(lines); }
    void set_cursor(std::optional<cch::tui::CursorPosition> cursor) { cursor_ = cursor; }

    [[nodiscard]] cch::support::Expected<cch::tui::RenderResult> render(std::size_t) override {
        return cch::tui::RenderResult{.lines = lines_};
    }

    void invalidate() override {}

    cch::tui::InputAdmissionOutcome handle_input(const cch::tui::InputEventVariant&) override {
        ++received;
        return cch::tui::InputAdmissionOutcome::Consumed;
    }

    void set_focused(bool focused) override { focused_ = focused; }
    [[nodiscard]] bool focused() const override { return focused_; }
    [[nodiscard]] std::optional<cch::tui::CursorPosition> cursor_location() const override { return cursor_; }

    std::size_t received{0};

private:
    std::vector<std::string> lines_;
    std::optional<cch::tui::CursorPosition> cursor_;
    bool focused_{false};
};

/// A terminal double whose autowrap disable fails once, after the alternate
/// buffer was already entered.
class FailingAutowrapTerminal final : public cch::tui::Terminal {
public:
    explicit FailingAutowrapTerminal(cch::tui::VirtualTerminalOptions options = {}) : inner_(std::move(options)) {}

    cch::support::ExpectedVoid start(
            cch::tui::TerminalInputSink input_sink, cch::tui::TerminalResizeSink resize_sink) override {
        return inner_.start(std::move(input_sink), std::move(resize_sink));
    }
    cch::support::ExpectedVoid stop() override { return inner_.stop(); }
    cch::tui::TerminalDimensions dimensions() const override { return inner_.dimensions(); }
    cch::tui::TerminalCapabilities capabilities() const override { return inner_.capabilities(); }
    cch::tui::TerminalModeState modes() const override { return inner_.modes(); }
    cch::support::ExpectedVoid clear_screen() override { return inner_.clear_screen(); }
    cch::support::ExpectedVoid write(std::string_view output) override { return inner_.write(output); }
    cch::support::ExpectedVoid set_cursor(cch::tui::CursorPosition position) override {
        return inner_.set_cursor(position);
    }
    cch::support::ExpectedVoid set_cursor_visible(bool visible) override { return inner_.set_cursor_visible(visible); }
    cch::support::ExpectedVoid set_alternate_screen(bool enabled) override {
        return inner_.set_alternate_screen(enabled);
    }
    cch::support::ExpectedVoid set_autowrap(bool enabled) override {
        if (!enabled && fail_autowrap) {
            fail_autowrap = false;
            return std::unexpected(cch::support::make_error(cch::support::ErrorCode::Busy, "injected autowrap fault"));
        }
        return inner_.set_autowrap(enabled);
    }
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

    bool fail_autowrap{true};
    cch::tui::VirtualTerminal inner_;
};

[[nodiscard]] std::string all_output(const cch::tui::VirtualTerminal& terminal) {
    std::string output;
    for (const auto& chunk : terminal.output())
        output += chunk;
    return output;
}

[[nodiscard]] std::size_t row_write_count(const cch::tui::VirtualTerminal& terminal, std::size_t from) {
    std::size_t count = 0;
    for (auto index = from; index < terminal.output().size(); ++index) {
        if (terminal.output()[index].find("\x1b[2K") != std::string::npos) ++count;
    }
    return count;
}

} // namespace

TEST_CASE("AltScreen enters the alternate buffer, disables autowrap, and restores the main buffer on stop",
        "[tui][alt-screen][issue997][spec]") {
    cch::tui::VirtualTerminal terminal({.columns = 10, .rows = 3});
    REQUIRE(terminal.seed_shell_content({"main", "buffer"}, cch::tui::CursorPosition{.column = 0, .row = 0}));

    cch::tui::AltScreen tui(terminal);
    auto child = std::make_unique<AltChild>(std::vector<std::string>{"alt"});
    REQUIRE(tui.add_child(std::move(child)));
    REQUIRE(tui.start());
    CHECK(terminal.modes().alternate_screen);
    CHECK_FALSE(terminal.modes().autowrap);
    CHECK(all_output(terminal).find("\x1b[?1049h") != std::string::npos);
    CHECK(all_output(terminal).find("\x1b[?7l") != std::string::npos);

    REQUIRE(tui.render());
    CHECK(terminal.screen().front() == "alt       ");

    REQUIRE(tui.stop());
    CHECK_FALSE(terminal.modes().alternate_screen);
    CHECK(terminal.modes().autowrap);
    CHECK(all_output(terminal).find("\x1b[?1049l") != std::string::npos);
    CHECK(all_output(terminal).find("\x1b[?7h") != std::string::npos);
    // The pre-existing main buffer returns exactly as it was.
    REQUIRE(terminal.screen().size() == 3);
    CHECK(terminal.screen()[0] == "main");
    CHECK(terminal.screen()[1] == "buffer");
}

TEST_CASE(
        "AltScreen paints an absolute differential viewport without scrollback", "[tui][alt-screen][issue997][spec]") {
    cch::tui::VirtualTerminal terminal({.columns = 8, .rows = 4});
    cch::tui::AltScreen tui(terminal);
    auto text = std::make_unique<cch::tui::Text>("one\ntwo\nthree", 0, 0);
    auto* text_pointer = text.get();
    REQUIRE(tui.add_child(std::move(text)));
    REQUIRE(tui.start());
    // A fresh viewport paints every row but never clears the buffer first.
    CHECK_FALSE(terminal.check_clear_screen_called());
    REQUIRE(tui.render());
    REQUIRE(terminal.screen().size() == 4);
    CHECK(terminal.screen()[0] == "one     ");
    CHECK(terminal.screen()[1] == "two     ");
    CHECK(terminal.screen()[2] == "three   ");
    CHECK(terminal.screen()[3] == "        ");
    CHECK(row_write_count(terminal, 0) == 4);
    CHECK(terminal.scrollback().empty());

    // Only the changed row is written; the unchanged rows are skipped.
    const auto before = terminal.output().size();
    text_pointer->set_text("one\nTWO\nthree");
    REQUIRE(tui.render());
    CHECK(row_write_count(terminal, before) == 1);
    bool wrote_changed_row{false};
    for (auto index = before; index < terminal.output().size(); ++index) {
        if (terminal.output()[index].find("\x1b[2;1H\x1b[2KTWO     ") != std::string::npos) wrote_changed_row = true;
    }
    CHECK(wrote_changed_row);
    CHECK(terminal.screen()[1] == "TWO     ");
    CHECK(terminal.scrollback().empty());
}

TEST_CASE("AltScreen clips content to the viewport and repaints across resizes", "[tui][alt-screen][issue997][spec]") {
    cch::tui::VirtualTerminal terminal({.columns = 6, .rows = 3});
    cch::tui::AltScreen tui(terminal);
    auto text = std::make_unique<cch::tui::Text>("one\ntwo\nthree\nfour\nfive", 0, 0);
    auto* text_pointer = text.get();
    REQUIRE(tui.add_child(std::move(text)));
    REQUIRE(tui.start());
    REQUIRE(tui.render());
    REQUIRE(terminal.screen().size() == 3);
    CHECK(terminal.screen()[2] == "three ");
    // Overflowing content is clipped at the viewport, never appended below it.
    CHECK(terminal.scrollback().empty());

    REQUIRE(terminal.inject_resize(cch::tui::TerminalDimensions{.columns = 6, .rows = 2}));
    REQUIRE(tui.render());
    REQUIRE(terminal.screen().size() == 2);
    CHECK(terminal.screen()[1] == "two   ");

    text_pointer->set_text("one\ntwo\nthree\nfour");
    REQUIRE(terminal.inject_resize(cch::tui::TerminalDimensions{.columns = 6, .rows = 4}));
    REQUIRE(tui.render());
    REQUIRE(terminal.screen().size() == 4);
    CHECK(terminal.screen()[3] == "four  ");
    CHECK(terminal.scrollback().empty());
}

TEST_CASE("AltScreen clears on shrink only when requested and after a render-state reset",
        "[tui][alt-screen][issue997][spec]") {
    cch::tui::VirtualTerminal terminal({.columns = 6, .rows = 4});
    cch::tui::AltScreen tui(terminal);
    REQUIRE(tui.add_child(std::make_unique<cch::tui::Text>("one\ntwo\nthree\nfour", 0, 0)));
    REQUIRE(tui.start());
    REQUIRE(tui.render());
    CHECK_FALSE(terminal.check_clear_screen_called());

    REQUIRE(terminal.inject_resize(cch::tui::TerminalDimensions{.columns = 6, .rows = 3}));
    REQUIRE(tui.render());
    CHECK_FALSE(terminal.check_clear_screen_called());

    tui.set_clear_on_shrink(true);
    REQUIRE(terminal.inject_resize(cch::tui::TerminalDimensions{.columns = 6, .rows = 2}));
    REQUIRE(tui.render());
    CHECK(terminal.check_clear_screen_called());

    // reset_render_state() is the -1 sentinel: the next frame clears and
    // repaints even though the terminal dimensions did not change.
    tui.reset_render_state();
    REQUIRE(tui.render());
    CHECK(terminal.check_clear_screen_called());
}

TEST_CASE("AltScreen composes and clamps the child cursor to the viewport", "[tui][alt-screen][issue997][spec]") {
    cch::tui::VirtualTerminal terminal({.columns = 8, .rows = 3});
    cch::tui::AltScreen tui(terminal);
    auto child = std::make_unique<AltChild>(std::vector<std::string>{"row"});
    auto* child_pointer = child.get();
    child_pointer->set_cursor(cch::tui::CursorPosition{.column = 20, .row = 9});
    REQUIRE(tui.add_child(std::move(child)));
    REQUIRE(tui.start());
    REQUIRE(tui.render());
    CHECK(terminal.modes().cursor_visible);
    CHECK(terminal.cursor().column == 8);
    CHECK(terminal.cursor().row == 2);

    child_pointer->set_cursor(std::nullopt);
    REQUIRE(tui.render());
    CHECK_FALSE(terminal.modes().cursor_visible);
}

TEST_CASE("AltScreen restores the main buffer when start fails mid-entry", "[tui][alt-screen][issue997][spec]") {
    FailingAutowrapTerminal terminal({.columns = 10, .rows = 3});
    REQUIRE(terminal.inner_.seed_shell_content({"main"}, cch::tui::CursorPosition{.column = 0, .row = 0}));

    cch::tui::AltScreen tui(terminal);
    REQUIRE(tui.add_child(std::make_unique<AltChild>(std::vector<std::string>{"alt"})));
    const auto started = tui.start();
    REQUIRE_FALSE(started);
    CHECK(started.error().code == cch::support::ErrorCode::Busy);
    // The already-entered alternate buffer was exited again and the terminal
    // was stopped, so the main buffer survives the failed start.
    CHECK_FALSE(terminal.modes().alternate_screen);
    CHECK(terminal.modes().autowrap);
    CHECK(terminal.inner_.screen().front() == "main");
    CHECK_FALSE(tui.render());
}

TEST_CASE("AltScreen shares children, fallback focus, typed input, and ordered raw listeners",
        "[tui][alt-screen][issue997][spec]") {
    cch::tui::VirtualTerminal terminal({.columns = 20, .rows = 4});
    cch::tui::AltScreen tui(terminal);
    auto child = std::make_unique<AltChild>(std::vector<std::string>{"input"});
    auto* child_pointer = child.get();
    REQUIRE(tui.add_child(std::move(child)));

    std::vector<std::string> seen;
    auto first = tui.add_raw_input_listener([&seen](std::string_view text) {
        seen.emplace_back(text);
        return cch::tui::RawInputListenerResult::replace(std::string{text} + "!");
    });
    auto second = tui.add_raw_input_listener([&seen](std::string_view text) {
        seen.emplace_back(text);
        return cch::tui::RawInputListenerResult::pass();
    });

    REQUIRE(tui.start());
    CHECK(tui.focus() == child_pointer);
    CHECK(child_pointer->focused());
    REQUIRE(terminal.inject_input("x"));
    CHECK(seen == std::vector<std::string>{"x", "x!"});
    CHECK(child_pointer->received == 2);

    // An empty-string rewrite still runs the rest of the chain but leaves
    // nothing to decode, so typed dispatch is skipped.
    std::size_t empty_rewrite_observers = 0;
    auto empty =
            tui.add_raw_input_listener([](std::string_view) { return cch::tui::RawInputListenerResult::replace(""); });
    auto observer = tui.add_raw_input_listener([&empty_rewrite_observers](std::string_view text) {
        ++empty_rewrite_observers;
        CHECK(text.empty());
        return cch::tui::RawInputListenerResult::pass();
    });
    const auto received_before = child_pointer->received;
    REQUIRE(terminal.inject_input("y"));
    CHECK(empty_rewrite_observers == 1);
    CHECK(child_pointer->received == received_before);

    // Consuming stops the chain: later listeners and typed dispatch do not run.
    std::size_t after_consumer = 0;
    auto consumer =
            tui.add_raw_input_listener([](std::string_view) { return cch::tui::RawInputListenerResult::consume(); });
    auto never = tui.add_raw_input_listener([&after_consumer](std::string_view) {
        ++after_consumer;
        return cch::tui::RawInputListenerResult::pass();
    });
    REQUIRE(terminal.inject_input("z"));
    CHECK(after_consumer == 0);
    CHECK(child_pointer->received == received_before);
    REQUIRE(tui.stop());
}

TEST_CASE("AltScreen streams color-scheme notifications and late color replies", "[tui][alt-screen][issue997][spec]") {
    cch::tui::VirtualTerminal terminal({.columns = 20, .rows = 4});
    cch::tui::AltScreen tui(terminal);
    tui.set_terminal_color_scheme_notifications(true);
    REQUIRE(tui.start());
    CHECK(all_output(terminal).find("\x1b[?2031h") != std::string::npos);

    std::atomic<int> late_calls{0};
    cch::tui::TerminalColors late_colors;
    auto future = tui.query_terminal_colors(cch::tui::TerminalColorsQueryOptions{
            .timeout = std::chrono::milliseconds(30),
            .on_late_reply =
                    [&](cch::tui::TerminalColors colors) {
                        ++late_calls;
                        late_colors = colors;
                    },
    });

    REQUIRE(terminal.inject_input("\x1b]10;#010203\x07"));
    const auto partial = future.get();
    REQUIRE(partial.foreground.has_value());
    CHECK(*partial.foreground == cch::tui::RgbColor{.red = 1, .green = 2, .blue = 3});
    CHECK_FALSE(partial.background.has_value());

    REQUIRE(terminal.inject_input("\x1b]11;#040506\x07"));
    CHECK(late_calls.load() >= 1);
    REQUIRE(late_colors.background.has_value());
    CHECK(*late_colors.background == cch::tui::RgbColor{.red = 4, .green = 5, .blue = 6});

    REQUIRE(tui.stop());
    CHECK(all_output(terminal).find("\x1b[?2031l") != std::string::npos);
}

TEST_CASE("AltScreen render-state transfer keeps the no-baseline and reset cases distinct",
        "[tui][alt-screen][issue997][spec]") {
    cch::tui::VirtualTerminal terminal({.columns = 8, .rows = 3});
    cch::tui::AltScreen tui(terminal);
    REQUIRE(tui.add_child(std::make_unique<cch::tui::Text>("one", 0, 0)));
    REQUIRE(tui.start());
    REQUIRE(tui.render());

    const auto captured = tui.capture_render_state();
    CHECK(captured.width == 8);
    CHECK(captured.height == 3);
    CHECK(captured.viewport_rows.size() == 3);

    // A committed baseline makes an unchanged frame write no rows.
    const auto before = terminal.output().size();
    REQUIRE(tui.render());
    CHECK(row_write_count(terminal, before) == 0);

    // A transferred baseline survives the next start (the #985 case).
    REQUIRE(tui.stop());
    REQUIRE(tui.restore_render_state(captured));
    REQUIRE(tui.start());
    CHECK_FALSE(terminal.check_clear_screen_called());
    const auto restored_before = terminal.output().size();
    REQUIRE(tui.render());
    CHECK(row_write_count(terminal, restored_before) == 0);

    // A construction-time baseline is not restorable while running.
    CHECK_FALSE(tui.restore_render_state(captured));
}

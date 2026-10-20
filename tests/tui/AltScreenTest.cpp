#include <cch/tui/AltScreen.hpp>
#include <cch/tui/Container.hpp>
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

TEST_CASE("AltScreen layout root can be taken and reinstalled without destroying its tree",
        "[tui][layout][issue1018][spec]") {
    cch::tui::VirtualTerminal terminal({.columns = 8, .rows = 2});
    cch::tui::AltScreen tui{terminal};
    auto child = std::make_unique<AltChild>(std::vector<std::string>{"input"});
    auto* child_pointer = child.get();
    auto root = std::make_unique<cch::tui::Container>();
    REQUIRE(root->add_child(std::move(child)));
    auto* root_pointer = root.get();
    tui.set_layout_root(std::move(root));
    REQUIRE(tui.set_focus(child_pointer));

    auto taken = tui.take_layout_root();
    REQUIRE(taken);
    CHECK(taken.get() == root_pointer);
    CHECK(tui.focus() == nullptr);
    tui.set_layout_root(std::move(taken));
    REQUIRE(tui.set_focus(child_pointer));
    CHECK(tui.focus() == child_pointer);
}

/// A component that renders one word row, ignores presses (so the fullscreen
/// root owns the selection gesture) and consumes clicks while recording the
/// delivered click count (pi `clickCount: this.lastClick?.count ?? 1`).
class SelectionClickProbe final : public cch::tui::Component, public cch::tui::MouseHandler {
public:
    SelectionClickProbe(std::string line, std::vector<std::size_t>& click_counts)
        : line_(std::move(line)), click_counts_(click_counts) {}

    [[nodiscard]] cch::support::Expected<cch::tui::RenderResult> render(std::size_t) override {
        return cch::tui::RenderResult{.lines = std::vector<std::string>{line_}};
    }

    void invalidate() override {}

    [[nodiscard]] std::optional<cch::tui::MouseEventResult> handle_mouse(const cch::tui::MouseEvent& event) override {
        if (event.type != cch::tui::MouseEventType::Click) return std::nullopt;
        click_counts_.push_back(event.click_count.value_or(0));
        return cch::tui::MouseEventResult{.handled = true, .render = false};
    }

private:
    std::string line_;
    std::vector<std::size_t>& click_counts_;
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
    cch::support::ExpectedVoid set_program_status(const cch::tui::ProgramStatus& status) override {
        return inner_.set_program_status(status);
    }
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

TEST_CASE("AltScreen selects styled wrapped CJK text, highlights cells, and copies through its sink",
        "[tui][alt-screen][selection][issue1006][spec]") {
    cch::tui::VirtualTerminal terminal({.columns = 6, .rows = 4});
    std::vector<std::string> copied;
    cch::tui::AltScreen tui(terminal,
            cch::tui::AltScreenOptions{.copy_on_select = false,
                    .copy_selection = [&copied](std::string_view text) -> cch::support::ExpectedVoid {
                        copied.emplace_back(text);
                        return {};
                    }});
    REQUIRE(tui.add_child(std::make_unique<cch::tui::Text>("\x1b[31mred界abc\x1b[0m", 0, 0)));
    REQUIRE(tui.start());
    REQUIRE(tui.render());
    REQUIRE(terminal.inject_input("\x1b[<0;2;1M"));
    REQUIRE(terminal.inject_input("\x1b[<32;2;2M"));
    REQUIRE(terminal.inject_input("\x1b[<0;2;2m"));
    REQUIRE(tui.render());

    CHECK(tui.has_active_selection());
    CHECK(all_output(terminal).find("\x1b[7m") != std::string::npos);
    const auto result = tui.copy_active_selection_to_clipboard();
    REQUIRE(result);
    CHECK(*result);
    REQUIRE(copied == std::vector<std::string>{"ed界\nab"});
    REQUIRE(tui.stop());
}

TEST_CASE("AltScreen keeps selection coordinates across content updates and focus changes",
        "[tui][alt-screen][selection][issue1006][spec]") {
    cch::tui::VirtualTerminal terminal({.columns = 20, .rows = 4});
    std::vector<std::string> copied;
    cch::tui::AltScreen tui(terminal,
            cch::tui::AltScreenOptions{.copy_on_select = false,
                    .copy_selection = [&copied](std::string_view text) -> cch::support::ExpectedVoid {
                        copied.emplace_back(text);
                        return {};
                    }});
    auto text = std::make_unique<cch::tui::Text>("alpha\nbeta\ngamma\ndelta", 0, 0);
    auto* text_pointer = text.get();
    REQUIRE(tui.add_child(std::move(text)));
    REQUIRE(tui.start());
    REQUIRE(tui.render());
    REQUIRE(terminal.inject_input("\x1b[<0;2;1M"));
    REQUIRE(terminal.inject_input("\x1b[<32;5;2M"));
    REQUIRE(terminal.inject_input("\x1b[<0;5;2m"));
    CHECK(tui.has_active_selection());

    text_pointer->set_text("omega\nsigma\ngamma\ndelta");
    REQUIRE(tui.render());
    const auto updated = tui.copy_active_selection_to_clipboard();
    REQUIRE(updated);
    CHECK(*updated);
    REQUIRE(copied == std::vector<std::string>{"mega\nsigma"});

    REQUIRE(terminal.inject_input("\x1b[O"));
    REQUIRE(terminal.inject_input("\x1b[I"));
    CHECK(tui.has_active_selection());
    REQUIRE(terminal.inject_input("\x1b[<0;2;3M"));
    REQUIRE(terminal.inject_input("\x1b[<0;2;3m"));
    CHECK_FALSE(tui.has_active_selection());
    REQUIRE(tui.stop());
}

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

TEST_CASE("AltScreen shows the end of overflowing content and repaints across resizes",
        "[tui][alt-screen][issue997][issue1000][spec]") {
    // Children mounted without a layout root are one document behind the
    // implicit follow-end viewport, so overflow shows the document's end and a
    // resize re-clamps that window (pi `tui-alt-screen.ts:262-270`,
    // `test/tui-alt-screen.test.ts:63-90`). #1000 replaced the earlier
    // top-clipped flow for this root.
    cch::tui::VirtualTerminal terminal({.columns = 6, .rows = 3});
    cch::tui::AltScreen tui(terminal);
    auto text = std::make_unique<cch::tui::Text>("one\ntwo\nthree\nfour\nfive", 0, 0);
    auto* text_pointer = text.get();
    REQUIRE(tui.add_child(std::move(text)));
    REQUIRE(tui.start());
    REQUIRE(tui.render());
    REQUIRE(terminal.screen().size() == 3);
    CHECK(terminal.screen()[0] == "three ");
    CHECK(terminal.screen()[2] == "five  ");
    // Overflowing content is clipped at the viewport, never appended below it.
    CHECK(terminal.scrollback().empty());

    REQUIRE(terminal.inject_resize(cch::tui::TerminalDimensions{.columns = 6, .rows = 2}));
    REQUIRE(tui.render());
    REQUIRE(terminal.screen().size() == 2);
    CHECK(terminal.screen()[0] == "four  ");
    CHECK(terminal.screen()[1] == "five  ");

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

TEST_CASE("AltScreen clamps the child cursor column and hides a row the frame does not paint",
        "[tui][alt-screen][issue997][issue1000][spec]") {
    cch::tui::VirtualTerminal terminal({.columns = 8, .rows = 3});
    cch::tui::AltScreen tui(terminal, true);
    auto child = std::make_unique<AltChild>(std::vector<std::string>{"row"});
    auto* child_pointer = child.get();
    child_pointer->set_cursor(cch::tui::CursorPosition{.column = 20, .row = 0});
    REQUIRE(tui.add_child(std::move(child)));
    REQUIRE(tui.start());
    REQUIRE(tui.render());
    CHECK(terminal.modes().cursor_visible);
    CHECK(terminal.cursor().column == 8);
    CHECK(terminal.cursor().row == 0);

    // #1000 routes the cursor through the frame, so a row the composed document
    // does not paint carries no cursor: it is not clamped into the viewport.
    child_pointer->set_cursor(cch::tui::CursorPosition{.column = 1, .row = 9});
    REQUIRE(tui.render());
    CHECK_FALSE(terminal.modes().cursor_visible);

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

TEST_CASE("AltScreen counts viewport repaints, writes, and the debug hook through the shared contract",
        "[tui][alt-screen][diagnostics][issue1016][spec]") {
    cch::tui::VirtualTerminal terminal({.columns = 6, .rows = 3});
    cch::tui::AltScreen tui(terminal);
    auto child = std::make_unique<AltChild>(std::vector<std::string>{"one", "two", "three"});
    auto* child_pointer = child.get();
    REQUIRE(tui.add_child(std::move(child)));

    std::size_t hook_calls = 0;
    tui.set_on_debug([&hook_calls] { ++hook_calls; });

    REQUIRE(tui.start());
    CHECK(tui.focus() == child_pointer);
    CHECK(tui.full_redraws() == 0);
    CHECK(tui.write_accounting() == cch::tui::TuiWriteAccounting{});

    // The first frame has no baseline: it repaints all three viewport rows, one
    // write per row, and counts one full-redraw attempt.
    REQUIRE(tui.render());
    CHECK(tui.full_redraws() == 1);
    CHECK(tui.write_accounting().attempted_writes == 3);
    CHECK(tui.write_accounting().committed_writes == 3);
    CHECK(tui.write_accounting().committed_bytes > 0);

    // A single changed row takes the differential path: one write, no attempt.
    child_pointer->set_lines({"one", "two", "four"});
    REQUIRE(tui.render());
    CHECK(tui.full_redraws() == 1);
    CHECK(tui.write_accounting().attempted_writes == 1);
    CHECK(tui.write_accounting().committed_writes == 1);

    // The reset sentinel is the clearing full-redraw path.
    tui.reset_render_state();
    REQUIRE(tui.render());
    CHECK(terminal.check_clear_screen_called());
    CHECK(tui.full_redraws() == 2);
    CHECK(tui.write_accounting().attempted_writes == 3);
    CHECK(tui.write_accounting().committed_writes == 3);

    // The debug chord is consumed by the hook before the focused child sees it.
    const auto received_before = child_pointer->received;
    REQUIRE(terminal.inject_input("\x1b[68;6u"));
    CHECK(hook_calls == 1);
    CHECK(child_pointer->received == received_before);
    REQUIRE(terminal.inject_input("x"));
    CHECK(child_pointer->received == received_before + 1);

    // A restart repaints from a fresh baseline under the same lifetime counter,
    // and the hook survives stop/start (pi keeps `onDebug`).
    REQUIRE(tui.stop());
    REQUIRE(tui.start());
    CHECK(tui.full_redraws() == 2);
    REQUIRE(tui.render());
    CHECK(tui.full_redraws() == 3);
    CHECK(tui.write_accounting().attempted_writes == 3);
    REQUIRE(terminal.inject_input("\x1b[68;6u"));
    CHECK(hook_calls == 2);

    REQUIRE(tui.stop());
}

TEST_CASE("AltScreen multi-click selection cycles word, line, and character granularity on a click clock",
        "[tui][alt-screen][selection][issue1007][spec]") {
    cch::tui::VirtualTerminal terminal({.columns = 20, .rows = 4});
    std::vector<std::string> copied;
    cch::tui::AltScreen tui(terminal,
            cch::tui::AltScreenOptions{.copy_on_select = false,
                    .copy_selection = [&copied](std::string_view text) -> cch::support::ExpectedVoid {
                        copied.emplace_back(text);
                        return {};
                    }});
    std::chrono::milliseconds now{0};
    tui.set_selection_click_clock([&now] { return now; });
    REQUIRE(tui.add_child(std::make_unique<cch::tui::Text>("one two", 0, 0)));
    REQUIRE(tui.start());
    REQUIRE(tui.render());
    const auto click = [&](std::ptrdiff_t x, std::ptrdiff_t advance_ms) {
        now += std::chrono::milliseconds{advance_ms};
        REQUIRE(terminal.inject_input("\x1b[<0;" + std::to_string(x) + ";1M"));
        REQUIRE(terminal.inject_input("\x1b[<0;" + std::to_string(x) + ";1m"));
    };
    const auto copy = [&]() -> std::optional<std::string> {
        const auto result = tui.copy_active_selection_to_clipboard();
        if (!result || !*result) return std::nullopt;
        return copied.back();
    };

    click(2, 1000); // First press: character granularity, no selection.
    CHECK_FALSE(copy());
    click(2, 150); // Second press on the same word: word granularity.
    CHECK(copy() == "one");
    click(2, 150); // Third press: the whole source row.
    CHECK(copy() == "one two");
    click(2, 150); // Fourth press wraps the cycle back to a plain click.
    CHECK_FALSE(copy());
    CHECK_FALSE(tui.has_active_selection());
    click(2, 150); // Fifth press starts the next word cycle.
    CHECK(copy() == "one");

    click(2, 600); // Past the frozen 500 ms interval the cycle resets.
    CHECK_FALSE(copy());
    click(2, 150);
    CHECK(copy() == "one");

    // The frozen interval boundary is inclusive: exactly 500 ms counts up to
    // line granularity over the whole row, one millisecond past it resets the
    // cycle (pi `<= DOUBLE_CLICK_INTERVAL_MS`).
    click(2, 500);
    CHECK(copy() == "one two");
    click(2, 501);
    CHECK_FALSE(copy());
    click(2, 150);
    CHECK(copy() == "one");

    click(7, 150);       // A different word starts a fresh cycle: its first press
    CHECK_FALSE(copy()); // selects nothing.
    click(7, 150);
    CHECK(copy() == "two");
    click(2, 150); // Back on the first word its own cycle starts over.
    CHECK_FALSE(copy());
    click(2, 150);
    CHECK(copy() == "one");
    REQUIRE(tui.stop());
}

TEST_CASE("AltScreen word selection chains joiner-adjacent segments and stops at boundary ends",
        "[tui][alt-screen][selection][issue1007][spec]") {
    cch::tui::VirtualTerminal terminal({.columns = 24, .rows = 4});
    std::vector<std::string> copied;
    cch::tui::AltScreen tui(terminal,
            cch::tui::AltScreenOptions{.copy_on_select = false,
                    .copy_selection = [&copied](std::string_view text) -> cch::support::ExpectedVoid {
                        copied.emplace_back(text);
                        return {};
                    }});
    std::chrono::milliseconds now{0};
    tui.set_selection_click_clock([&now] { return now; });
    REQUIRE(tui.add_child(std::make_unique<cch::tui::Text>("a-b/c 12.45 中x", 0, 0)));
    REQUIRE(tui.start());
    REQUIRE(tui.render());
    const auto double_click = [&](std::ptrdiff_t x) {
        // Each call starts a fresh cycle: the previous one dies past the
        // frozen interval before the first press.
        now += std::chrono::milliseconds{600};
        REQUIRE(terminal.inject_input("\x1b[<0;" + std::to_string(x) + ";1M"));
        REQUIRE(terminal.inject_input("\x1b[<0;" + std::to_string(x) + ";1m"));
        now += std::chrono::milliseconds{150};
        REQUIRE(terminal.inject_input("\x1b[<0;" + std::to_string(x) + ";1M"));
        REQUIRE(terminal.inject_input("\x1b[<0;" + std::to_string(x) + ";1m"));
        const auto result = tui.copy_active_selection_to_clipboard();
        REQUIRE(result);
        REQUIRE(*result);
        return copied.back();
    };

    // pi `canJoin` chains words across a joiner on either side, so clicking
    // the word, the `-`, or the `/` selects the same joined run.
    CHECK(double_click(1) == "a-b/c"); // 'a'
    CHECK(double_click(2) == "a-b/c"); // '-'
    CHECK(double_click(4) == "a-b/c"); // '/'
    // Numeric punctuation inside one word segment stays one word.
    CHECK(double_click(7) == "12.45");
    // The dictionary-script character is its own word, and the boundary end
    // keeps the trailing letter out of the copied text.
    CHECK(double_click(13) == "中");
    CHECK(double_click(15) == "x");
    REQUIRE(tui.stop());
}

TEST_CASE("AltScreen word selection copies whole dictionary-script runs like the frozen segmenter",
        "[tui][alt-screen][selection][issue1007][spec]") {
    cch::tui::VirtualTerminal terminal({.columns = 40, .rows = 4});
    std::vector<std::string> copied;
    cch::tui::AltScreen tui(terminal,
            cch::tui::AltScreenOptions{.copy_on_select = false,
                    .copy_selection = [&copied](std::string_view text) -> cch::support::ExpectedVoid {
                        copied.emplace_back(text);
                        return {};
                    }});
    std::chrono::milliseconds now{0};
    tui.set_selection_click_clock([&now] { return now; });
    // One line: 日本語 (cols 1-6), a space, こんにちは (8-17), a space, then
    // สวัสดี (19-22) and ครับ (23-25) — the Thai combining graphemes are
    // narrow (one cell each) in both pi's `visibleWidth` and the pinned width
    // table, which the frozen `multiclick-dictionary-script-runs` evidence
    // pins. pi segments words with `Intl.Segmenter` (ICU); the C++ side
    // reuses the pinned ICU breaker, so multi-character Han, Hiragana and
    // Thai runs copy whole like the frozen evidence instead of one character
    // at a time.
    REQUIRE(tui.add_child(std::make_unique<cch::tui::Text>("日本語 こんにちは สวัสดีครับ", 0, 0)));
    REQUIRE(tui.start());
    REQUIRE(tui.render());
    const auto double_click = [&](std::ptrdiff_t x) {
        // First press of the pair: a plain character click.
        now += std::chrono::milliseconds{600};
        REQUIRE(terminal.inject_input("\x1b[<0;" + std::to_string(x) + ";1M"));
        now += std::chrono::milliseconds{150};
        REQUIRE(terminal.inject_input("\x1b[<0;" + std::to_string(x) + ";1m"));
        // Second press within the frozen 500 ms interval: word granularity.
        REQUIRE(terminal.inject_input("\x1b[<0;" + std::to_string(x) + ";1M"));
        REQUIRE(terminal.inject_input("\x1b[<0;" + std::to_string(x) + ";1m"));
        const auto result = tui.copy_active_selection_to_clipboard();
        REQUIRE(result);
        REQUIRE(*result);
        return copied.back();
    };

    // Han run: columns 1-6. Hiragana run: columns 8-17.
    CHECK(double_click(2) == "日本語");
    CHECK(double_click(5) == "日本語");
    CHECK(double_click(8) == "こんにちは");
    CHECK(double_click(13) == "こんにちは");
    // Thai runs: narrow graphemes, one dictionary word at a time like frozen pi.
    CHECK(double_click(21) == "สวัสดี");
    CHECK(double_click(24) == "ครับ");
    REQUIRE(tui.stop());
}

TEST_CASE(
        "AltScreen zero-width graphemes occupy no selection columns", "[tui][alt-screen][selection][issue1007][spec]") {
    cch::tui::VirtualTerminal terminal({.columns = 20, .rows = 3});
    std::vector<std::string> copied;
    cch::tui::AltScreen tui(terminal,
            cch::tui::AltScreenOptions{.copy_on_select = false,
                    .copy_selection = [&copied](std::string_view text) -> cch::support::ExpectedVoid {
                        copied.emplace_back(text);
                        return {};
                    }});
    std::chrono::milliseconds now{0};
    tui.set_selection_click_clock([&now] { return now; });
    // U+200B ZERO WIDTH SPACE: pi gives it zero cells (`visibleWidth`), so the
    // columns after it line up with the plain-text offsets.
    REQUIRE(tui.add_child(std::make_unique<cch::tui::Text>("中\xE2\x80\x8Bx", 0, 0)));
    REQUIRE(tui.start());
    REQUIRE(tui.render());
    const auto double_click = [&](std::ptrdiff_t x) {
        now += std::chrono::milliseconds{600};
        REQUIRE(terminal.inject_input("\x1b[<0;" + std::to_string(x) + ";1M"));
        REQUIRE(terminal.inject_input("\x1b[<0;" + std::to_string(x) + ";1m"));
        now += std::chrono::milliseconds{150};
        REQUIRE(terminal.inject_input("\x1b[<0;" + std::to_string(x) + ";1M"));
        REQUIRE(terminal.inject_input("\x1b[<0;" + std::to_string(x) + ";1m"));
        const auto result = tui.copy_active_selection_to_clipboard();
        REQUIRE(result);
        REQUIRE(*result);
        return copied.back();
    };

    // Columns: 中 spans 1-2, the zero-width space owns none, x lands on 3.
    CHECK(double_click(1) == "中");
    CHECK(double_click(2) == "中");
    // The click on the zero-width column selects the following letter, unlike
    // the old width-1 clamp that swallowed it into an empty segment. The copy
    // includes the zero-width grapheme exactly like pi's sliceWithWidth (a
    // zero-width grapheme at the slice start satisfies `currentCol >= startCol`).
    CHECK(double_click(3) == "\xE2\x80\x8Bx");
    REQUIRE(tui.stop());
}

TEST_CASE(
        "AltScreen ignores an empty selection click clock override", "[tui][alt-screen][selection][issue1007][spec]") {
    cch::tui::VirtualTerminal terminal({.columns = 20, .rows = 3});
    std::vector<std::string> copied;
    cch::tui::AltScreen tui(terminal,
            cch::tui::AltScreenOptions{.copy_on_select = false,
                    .copy_selection = [&copied](std::string_view text) -> cch::support::ExpectedVoid {
                        copied.emplace_back(text);
                        return {};
                    }});
    std::chrono::milliseconds now{0};
    tui.set_selection_click_clock([&now] { return now; });
    REQUIRE(tui.add_child(std::make_unique<cch::tui::Text>("one two", 0, 0)));
    REQUIRE(tui.start());
    REQUIRE(tui.render());

    // An empty callable would be invoked on the next press (undefined
    // behavior); the setter ignores it and the injected clock keeps driving.
    tui.set_selection_click_clock(std::move_only_function<std::chrono::milliseconds()>{});
    now += std::chrono::milliseconds{600};
    REQUIRE(terminal.inject_input("\x1b[<0;2;1M"));
    REQUIRE(terminal.inject_input("\x1b[<0;2;1m"));
    now += std::chrono::milliseconds{150};
    REQUIRE(terminal.inject_input("\x1b[<0;2;1M"));
    REQUIRE(terminal.inject_input("\x1b[<0;2;1m"));
    const auto result = tui.copy_active_selection_to_clipboard();
    REQUIRE(result);
    REQUIRE(*result);
    CHECK(copied.back() == "one");
    REQUIRE(tui.stop());
}

TEST_CASE("AltScreen line-granularity drags expand by whole lines across an empty row",
        "[tui][alt-screen][selection][issue1007][spec]") {
    cch::tui::VirtualTerminal terminal({.columns = 20, .rows = 5});
    std::vector<std::string> copied;
    cch::tui::AltScreen tui(terminal,
            cch::tui::AltScreenOptions{.copy_on_select = false,
                    .copy_selection = [&copied](std::string_view text) -> cch::support::ExpectedVoid {
                        copied.emplace_back(text);
                        return {};
                    }});
    std::chrono::milliseconds now{0};
    tui.set_selection_click_clock([&now] { return now; });
    REQUIRE(tui.add_child(std::make_unique<cch::tui::Text>("alpha\n\nomega", 0, 0)));
    REQUIRE(tui.start());
    REQUIRE(tui.render());
    const auto copy = [&]() -> std::optional<std::string> {
        const auto result = tui.copy_active_selection_to_clipboard();
        if (!result || !*result) return std::nullopt;
        return copied.back();
    };

    now += std::chrono::milliseconds{1000};
    REQUIRE(terminal.inject_input("\x1b[<0;2;1M")); // First press: no selection.
    REQUIRE(terminal.inject_input("\x1b[<0;2;1m"));
    CHECK_FALSE(copy());
    now += std::chrono::milliseconds{150};
    REQUIRE(terminal.inject_input("\x1b[<0;2;1M")); // Second press: the word.
    REQUIRE(terminal.inject_input("\x1b[<0;2;1m"));
    CHECK(copy() == "alpha");
    // Third press: line granularity over the whole source row, held for a drag.
    // pi's line range is infallible (an empty row is a zero-width range), so
    // dragging across the empty row expands by whole lines, not characters.
    now += std::chrono::milliseconds{150};
    REQUIRE(terminal.inject_input("\x1b[<0;2;1M"));
    REQUIRE(terminal.inject_input("\x1b[<32;2;2M")); // ... across the empty row ...
    CHECK(copy() == "alpha\n");
    REQUIRE(terminal.inject_input("\x1b[<32;2;3M")); // ... onto the last row.
    CHECK(copy() == "alpha\n\nomega");
    REQUIRE(terminal.inject_input("\x1b[<0;2;3m"));
    CHECK(copy() == "alpha\n\nomega");
    REQUIRE(tui.stop());
}

TEST_CASE("AltScreen selection release clicks report the multi-click count to components",
        "[tui][alt-screen][selection][issue1007][spec]") {
    cch::tui::VirtualTerminal terminal({.columns = 20, .rows = 3});
    std::vector<std::size_t> click_counts;
    auto probe = std::make_unique<SelectionClickProbe>("one two", click_counts);
    cch::tui::AltScreen tui(terminal);
    std::chrono::milliseconds now{0};
    tui.set_selection_click_clock([&now] { return now; });
    REQUIRE(tui.add_child(std::move(probe)));
    REQUIRE(tui.start());
    REQUIRE(tui.render());

    // The probe renders the word and consumes clicks, but never presses, so
    // the fullscreen root owns the gesture and its multi-click bookkeeping.
    // The release click only fires when the press lands on the range's first
    // cell (pi compares the anchor with the release point), so click the 'o'.
    now += std::chrono::milliseconds{600};
    REQUIRE(terminal.inject_input("\x1b[<0;1;1M"));
    REQUIRE(terminal.inject_input("\x1b[<0;1;1m"));
    now += std::chrono::milliseconds{150};
    REQUIRE(terminal.inject_input("\x1b[<0;1;1M"));
    REQUIRE(terminal.inject_input("\x1b[<0;1;1m"));
    // pi delivers `clickCount: this.lastClick?.count ?? 1` on the release
    // click: the second release reports the recorded word-click count of 2.
    CHECK(click_counts == std::vector<std::size_t>{1, 2});
    REQUIRE(tui.stop());
}

TEST_CASE("AltScreen word-granularity drags expand by whole words and reverse at the initial range",
        "[tui][alt-screen][selection][issue1007][spec]") {
    cch::tui::VirtualTerminal terminal({.columns = 24, .rows = 4});
    std::vector<std::string> copied;
    cch::tui::AltScreen tui(terminal,
            cch::tui::AltScreenOptions{.copy_on_select = false,
                    .copy_selection = [&copied](std::string_view text) -> cch::support::ExpectedVoid {
                        copied.emplace_back(text);
                        return {};
                    }});
    std::chrono::milliseconds now{0};
    tui.set_selection_click_clock([&now] { return now; });
    REQUIRE(tui.add_child(std::make_unique<cch::tui::Text>("one two three", 0, 0)));
    REQUIRE(tui.start());
    REQUIRE(tui.render());
    const auto copy = [&]() -> std::optional<std::string> {
        const auto result = tui.copy_active_selection_to_clipboard();
        if (!result || !*result) return std::nullopt;
        return copied.back();
    };

    now += std::chrono::milliseconds{150};
    REQUIRE(terminal.inject_input("\x1b[<0;5;1M"));
    REQUIRE(terminal.inject_input("\x1b[<0;5;1m"));
    CHECK_FALSE(copy());
    // Double-click "two" and drag left into "one": the focus snaps to whole
    // words while the initial word keeps its far end anchored.
    now += std::chrono::milliseconds{150};
    REQUIRE(terminal.inject_input("\x1b[<0;5;1M"));
    REQUIRE(terminal.inject_input("\x1b[<32;1;1M"));
    CHECK(tui.has_active_selection());
    CHECK(copy() == "one two");
    // Dragging past the initial range reverses the anchor instead of
    // crossing it (pi `updateSelectionFocus`).
    REQUIRE(terminal.inject_input("\x1b[<32;13;1M"));
    CHECK(copy() == "two three");
    REQUIRE(terminal.inject_input("\x1b[<0;13;1m"));
    CHECK(copy() == "two three");
    // A drag press clears the click history: the next press is a fresh click.
    now += std::chrono::milliseconds{150};
    REQUIRE(terminal.inject_input("\x1b[<0;5;1M"));
    REQUIRE(terminal.inject_input("\x1b[<0;5;1m"));
    CHECK_FALSE(copy());
    REQUIRE(tui.stop());
}

TEST_CASE(
        "AltScreen opens OSC 8 links only on a matching character click", "[tui][alt-screen][mouse][issue1009][spec]") {
    cch::tui::VirtualTerminal terminal({.columns = 30, .rows = 2});
    std::vector<std::string> opened;
    cch::tui::AltScreen tui(terminal);
    std::chrono::milliseconds now{0};
    tui.set_selection_click_clock([&now] { return now; });
    tui.set_link_open_sink([&opened](std::string_view url) noexcept { opened.emplace_back(url); });
    REQUIRE(tui.add_child(std::make_unique<cch::tui::Text>(
            "\x1b]8;;https://example.com/alpha\x1b\\alpha\x1b]8;;\x1b\\ plain", 0, 0)));
    REQUIRE(tui.start());
    REQUIRE(tui.render());

    REQUIRE(terminal.inject_input("\x1b[<0;2;1M"));
    REQUIRE(terminal.inject_input("\x1b[<3;2;1m"));
    CHECK(opened == std::vector<std::string>{"https://example.com/alpha"});

    now += std::chrono::milliseconds{600};
    REQUIRE(terminal.inject_input("\x1b[<0;2;1M"));
    REQUIRE(terminal.inject_input("\x1b[<32;3;1M"));
    REQUIRE(terminal.inject_input("\x1b[<0;3;1m"));
    CHECK(opened == std::vector<std::string>{"https://example.com/alpha"});

    now += std::chrono::milliseconds{600};
    REQUIRE(terminal.inject_input("\x1b[<0;2;1M"));
    REQUIRE(terminal.inject_input("\x1b[<0;2;1m"));
    // The first click of a double-click pair opens on its release. The
    // count-two press starts word selection and has no pending URL.
    CHECK(opened == std::vector<std::string>{"https://example.com/alpha", "https://example.com/alpha"});
    REQUIRE(tui.stop());
}

TEST_CASE("AltScreen routes secondary-button presses to its paste sink", "[tui][alt-screen][mouse][issue1009][spec]") {
    cch::tui::VirtualTerminal terminal({.columns = 20, .rows = 2});
    std::vector<int> paste_buttons;
    cch::tui::AltScreen tui(terminal);
    tui.set_right_click_paste_sink([&paste_buttons]() -> cch::support::ExpectedVoid {
        paste_buttons.push_back(1);
        return {};
    });
    REQUIRE(tui.add_child(std::make_unique<cch::tui::Text>("alpha beta", 0, 0)));
    REQUIRE(tui.start());
    REQUIRE(tui.render());

    REQUIRE(terminal.inject_input("\x1b[<2;3;1M"));
    CHECK(paste_buttons.size() == 1);
    CHECK_FALSE(tui.has_active_selection());
    REQUIRE(terminal.inject_input("\x1b[<2;3;1m"));
    REQUIRE(terminal.inject_input("\x1b[<34;3;1M"));
    CHECK(paste_buttons.size() == 1);

    std::size_t declines = 0;
    tui.set_right_click_paste_sink([&declines]() -> cch::support::ExpectedVoid {
        ++declines;
        return std::unexpected(cch::support::make_error(cch::support::ErrorCode::Unknown, "paste declined"));
    });
    REQUIRE(terminal.inject_input("\x1b[<2;4;1M"));
    CHECK(declines == 1);
    CHECK(paste_buttons.size() == 1);
    CHECK_FALSE(tui.has_active_selection());

    REQUIRE(terminal.inject_input("\x1b[<6;5;1M"));
    REQUIRE(terminal.inject_input("\x1b[<18;6;1M"));
    CHECK(declines == 1);
    CHECK(paste_buttons.size() == 1);
    CHECK_FALSE(tui.has_active_selection());
    REQUIRE(tui.stop());
}

TEST_CASE("AltScreen preserves a pending OSC 8 link through pure pointer motion",
        "[tui][alt-screen][mouse][issue1009][spec]") {
    cch::tui::VirtualTerminal terminal({.columns = 20, .rows = 2});
    std::vector<std::string> opened;
    cch::tui::AltScreen tui(terminal);
    tui.set_link_open_sink([&opened](std::string_view url) noexcept { opened.emplace_back(url); });
    REQUIRE(tui.add_child(std::make_unique<cch::tui::Text>(
            "\x1b]8;;https://example.com/motion\x1b\\alpha\x1b]8;;\x1b\\ plain", 0, 0)));
    REQUIRE(tui.start());
    REQUIRE(tui.render());

    REQUIRE(terminal.inject_input("\x1b[<0;2;1M"));
    REQUIRE(terminal.inject_input("\x1b[<35;3;1M"));
    REQUIRE(terminal.inject_input("\x1b[<35;2;1M"));
    REQUIRE(terminal.inject_input("\x1b[<3;2;1m"));
    CHECK(opened == std::vector<std::string>{"https://example.com/motion"});
    REQUIRE(tui.stop());
}

TEST_CASE("AltScreen cancels a multi-click press on focus loss and drops its click history",
        "[tui][alt-screen][selection][issue1007][spec]") {
    cch::tui::VirtualTerminal terminal({.columns = 20, .rows = 4});
    std::vector<std::string> copied;
    cch::tui::AltScreen tui(terminal,
            cch::tui::AltScreenOptions{.copy_on_select = false,
                    .copy_selection = [&copied](std::string_view text) -> cch::support::ExpectedVoid {
                        copied.emplace_back(text);
                        return {};
                    }});
    std::chrono::milliseconds now{0};
    tui.set_selection_click_clock([&now] { return now; });
    REQUIRE(tui.add_child(std::make_unique<cch::tui::Text>("alpha beta", 0, 0)));
    REQUIRE(tui.start());
    REQUIRE(tui.render());
    const auto copy = [&]() -> std::optional<std::string> {
        const auto result = tui.copy_active_selection_to_clipboard();
        if (!result || !*result) return std::nullopt;
        return copied.back();
    };
    const auto click = [&](std::ptrdiff_t x, std::ptrdiff_t advance_ms) {
        now += std::chrono::milliseconds{advance_ms};
        REQUIRE(terminal.inject_input("\x1b[<0;" + std::to_string(x) + ";1M"));
        REQUIRE(terminal.inject_input("\x1b[<0;" + std::to_string(x) + ";1m"));
    };

    click(7, 1000);                                 // "beta", first press.
    REQUIRE(terminal.inject_input("\x1b[<0;7;1M")); // Second press held...
    REQUIRE(terminal.inject_input("\x1b[O"));       // ...until focus loss cancels it.
    CHECK_FALSE(tui.has_active_selection());
    REQUIRE(terminal.inject_input("\x1b[I"));
    REQUIRE(terminal.inject_input("\x1b[<0;7;1m"));
    CHECK_FALSE(copy());
    click(7, 150); // The cancelled press also dropped the click history.
    CHECK_FALSE(copy());
    click(7, 150);
    CHECK(copy() == "beta");
    REQUIRE(tui.stop());
}

TEST_CASE("AltScreen stop preserve_screen falls back to ordinary stop on unsupported terminal",
        "[tui][alt-screen][atomic][stop][fallback][issue1019][spec]") {
    class UnsupportedAtomicTerminal final : public cch::tui::Terminal {
    public:
        explicit UnsupportedAtomicTerminal(cch::tui::VirtualTerminal& inner) : inner_(inner) {}
        using Terminal::stop;
        cch::support::ExpectedVoid start(cch::tui::TerminalInputSink in, cch::tui::TerminalResizeSink rsz) override {
            return inner_.start(std::move(in), std::move(rsz));
        }
        // Inherits default stop(TerminalStopOptions) which returns Validation error for final_alternate_restore
        cch::support::ExpectedVoid stop() override {
            ++fallback_stop_calls;
            return inner_.stop();
        }
        cch::tui::TerminalDimensions dimensions() const override { return inner_.dimensions(); }
        cch::tui::TerminalCapabilities capabilities() const override { return inner_.capabilities(); }
        cch::tui::TerminalModeState modes() const override { return inner_.modes(); }
        cch::support::ExpectedVoid clear_screen() override { return inner_.clear_screen(); }
        cch::support::ExpectedVoid write(std::string_view s) override { return inner_.write(s); }
        cch::support::ExpectedVoid set_cursor(cch::tui::CursorPosition p) override { return inner_.set_cursor(p); }
        cch::support::ExpectedVoid set_cursor_visible(bool v) override { return inner_.set_cursor_visible(v); }
        cch::support::ExpectedVoid set_alternate_screen(bool a) override { return inner_.set_alternate_screen(a); }
        cch::support::ExpectedVoid set_autowrap(bool w) override { return inner_.set_autowrap(w); }
        cch::support::ExpectedVoid set_scroll_margins(std::size_t t, std::size_t b) override {
            return inner_.set_scroll_margins(t, b);
        }
        cch::support::ExpectedVoid reset_scroll_margins() override { return inner_.reset_scroll_margins(); }
        cch::support::ExpectedVoid begin_synchronized_update() override { return inner_.begin_synchronized_update(); }
        cch::support::ExpectedVoid end_synchronized_update() override { return inner_.end_synchronized_update(); }
        cch::support::ExpectedVoid set_title(std::string_view t) override { return inner_.set_title(t); }
        cch::support::ExpectedVoid set_progress(bool p) override { return inner_.set_progress(p); }
        cch::support::ExpectedVoid set_program_status(const cch::tui::ProgramStatus& status) override {
            return inner_.set_program_status(status);
        }
        cch::support::ExpectedVoid drain_input(std::chrono::milliseconds a, std::chrono::milliseconds b) override {
            return inner_.drain_input(a, b);
        }
        cch::support::ExpectedVoid enter_output_only() override { return inner_.enter_output_only(); }
        cch::support::ExpectedVoid leave_output_only() override { return inner_.leave_output_only(); }

        std::size_t fallback_stop_calls{0};

    private:
        cch::tui::VirtualTerminal& inner_;
    };

    cch::tui::VirtualTerminal vt({.columns = 40, .rows = 10});
    UnsupportedAtomicTerminal term(vt);
    cch::tui::AltScreen screen(term);
    REQUIRE(screen.start());

    // When preserve_screen=true is requested on a terminal that does not support
    // final_alternate_restore, AltScreen must invoke fallback stop() so input
    // and raw mode are restored, but return the Validation error (no false capability success).
    auto res = screen.stop(cch::tui::TuiStopOptions{.preserve_screen = true});
    REQUIRE_FALSE(res.has_value());
    CHECK(res.error().code == cch::support::ErrorCode::Validation);
    CHECK(term.fallback_stop_calls == 1);

    // Terminal was properly stopped by the fallback, so starting again succeeds
    REQUIRE(screen.start());
    REQUIRE(screen.stop());
}

TEST_CASE("AltScreen stop aggregates multiple errors from pre-steps and stop",
        "[tui][alt-screen][atomic][stop][error_aggregation][issue1019][spec]") {
    class FailingPreStepTerminal final : public cch::tui::Terminal {
    public:
        explicit FailingPreStepTerminal(cch::tui::VirtualTerminal& inner) : inner_(inner) {}
        cch::support::ExpectedVoid start(cch::tui::TerminalInputSink in, cch::tui::TerminalResizeSink rsz) override {
            return inner_.start(std::move(in), std::move(rsz));
        }
        using Terminal::stop;
        cch::support::ExpectedVoid stop() override { return inner_.stop(); }
        cch::support::ExpectedVoid stop(cch::tui::TerminalStopOptions) override {
            return std::unexpected(
                    cch::support::make_error(cch::support::ErrorCode::Process, "simulated atomic stop failure"));
        }
        cch::tui::TerminalDimensions dimensions() const override { return inner_.dimensions(); }
        cch::tui::TerminalCapabilities capabilities() const override { return inner_.capabilities(); }
        cch::tui::TerminalModeState modes() const override { return inner_.modes(); }
        cch::support::ExpectedVoid clear_screen() override { return inner_.clear_screen(); }
        cch::support::ExpectedVoid write(std::string_view s) override {
            if (s.find("1000l") != std::string_view::npos) {
                return std::unexpected(
                        cch::support::make_error(cch::support::ErrorCode::Process, "simulated mouse disable failure"));
            }
            return inner_.write(s);
        }
        cch::support::ExpectedVoid set_cursor(cch::tui::CursorPosition p) override { return inner_.set_cursor(p); }
        cch::support::ExpectedVoid set_cursor_visible(bool v) override { return inner_.set_cursor_visible(v); }
        cch::support::ExpectedVoid set_alternate_screen(bool a) override { return inner_.set_alternate_screen(a); }
        cch::support::ExpectedVoid set_autowrap(bool w) override { return inner_.set_autowrap(w); }
        cch::support::ExpectedVoid set_scroll_margins(std::size_t t, std::size_t b) override {
            return inner_.set_scroll_margins(t, b);
        }
        cch::support::ExpectedVoid reset_scroll_margins() override { return inner_.reset_scroll_margins(); }
        cch::support::ExpectedVoid begin_synchronized_update() override { return inner_.begin_synchronized_update(); }
        cch::support::ExpectedVoid end_synchronized_update() override { return inner_.end_synchronized_update(); }
        cch::support::ExpectedVoid set_title(std::string_view t) override { return inner_.set_title(t); }
        cch::support::ExpectedVoid set_progress(bool p) override { return inner_.set_progress(p); }
        cch::support::ExpectedVoid set_program_status(const cch::tui::ProgramStatus& status) override {
            return inner_.set_program_status(status);
        }
        cch::support::ExpectedVoid drain_input(std::chrono::milliseconds a, std::chrono::milliseconds b) override {
            return inner_.drain_input(a, b);
        }
        cch::support::ExpectedVoid enter_output_only() override { return inner_.enter_output_only(); }
        cch::support::ExpectedVoid leave_output_only() override { return inner_.leave_output_only(); }

    private:
        cch::tui::VirtualTerminal& inner_;
    };

    cch::tui::VirtualTerminal vt({.columns = 40, .rows = 10});
    FailingPreStepTerminal term(vt);
    cch::tui::AltScreen screen(term);
    REQUIRE(screen.start());

    auto res = screen.stop(cch::tui::TuiStopOptions{.preserve_screen = true});
    REQUIRE_FALSE(res.has_value());
    // Primary error (mouse disable failure) and secondary error (atomic stop failure)
    // are both recorded in detail by combine_errors.
    CHECK(res.error().message.find("AltScreen stop encountered multiple failures") != std::string::npos);
    CHECK(res.error().detail.find("mouse disable failure") != std::string::npos);
    CHECK(res.error().detail.find("atomic stop failure") != std::string::npos);
}

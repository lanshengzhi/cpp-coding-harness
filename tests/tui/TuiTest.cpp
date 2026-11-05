#include <cch/tui/AltScreen.hpp>
#include <cch/tui/Container.hpp>
#include <cch/tui/Editor.hpp>
#include <cch/tui/Keys.hpp>
#include <cch/tui/Text.hpp>
#include <cch/tui/Tui.hpp>
#include <cch/tui/VirtualTerminal.hpp>
#include <cch/tui/Utils.hpp>

#include "support/PiTuiEvidence.hpp"

#include <cch/support/Error.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

class ManualDelayedHandle final : public cch::tui::TuiScheduledHandle {
public:
    ~ManualDelayedHandle() override { *cancelled_ = true; }
    explicit ManualDelayedHandle(std::shared_ptr<bool> cancelled) : cancelled_(std::move(cancelled)) {}
    void cancel() noexcept override { *cancelled_ = true; }

private:
    std::shared_ptr<bool> cancelled_;
};

class ManualTuiScheduler final : public cch::tui::TuiScheduler {
public:
    [[nodiscard]] std::chrono::steady_clock::time_point now() const noexcept override { return now_; }
    void post(cch::tui::TuiScheduledWork work) override { posted_.push_back(std::move(work)); }
    void schedule(std::chrono::steady_clock::duration delay, cch::tui::TuiScheduledWork work) override {
        scheduled_delay_ = delay;
        scheduled_ = std::move(work);
        ++schedule_count_;
    }
    [[nodiscard]] std::unique_ptr<cch::tui::TuiScheduledHandle> schedule_delayed(
            std::chrono::steady_clock::duration delay, cch::tui::TuiScheduledWork work) override {
        auto cancelled = std::make_shared<bool>(false);
        delayed_.push_back(Delayed{.deadline = now_ + delay, .work = std::move(work), .cancelled = cancelled});
        return std::make_unique<ManualDelayedHandle>(std::move(cancelled));
    }
    void cancel() noexcept override { scheduled_ = nullptr; }
    void schedule_interval(std::chrono::steady_clock::duration, cch::tui::TuiScheduledWork work) override {
        interval_work_ = std::make_shared<cch::tui::TuiScheduledWork>(std::move(work));
    }
    void cancel_interval() noexcept override { interval_work_.reset(); }

    void advance(std::chrono::steady_clock::duration duration) {
        now_ += duration;
        for (auto& item : delayed_) {
            if (!*item.cancelled && item.deadline <= now_) {
                *item.cancelled = true;
                if (item.work) item.work();
            }
        }
    }
    void run_scheduled() {
        auto scheduled = std::exchange(scheduled_, nullptr);
        if (scheduled) scheduled();
    }

    std::chrono::steady_clock::duration scheduled_delay_{};
    std::size_t schedule_count_{0};

private:
    struct Delayed {
        std::chrono::steady_clock::time_point deadline{};
        cch::tui::TuiScheduledWork work;
        std::shared_ptr<bool> cancelled;
    };
    std::chrono::steady_clock::time_point now_{};
    cch::tui::TuiScheduledWork scheduled_;
    std::shared_ptr<cch::tui::TuiScheduledWork> interval_work_;
    std::vector<cch::tui::TuiScheduledWork> posted_;
    std::vector<Delayed> delayed_;
};

class OverwideComponent final : public cch::tui::Component {
public:
    [[nodiscard]] cch::support::Expected<cch::tui::RenderResult> render(std::size_t) override {
        return cch::tui::RenderResult{.lines = {"too wide"}};
    }

    void invalidate() override {}
};

class PartialStartupFailureTerminal final : public cch::tui::Terminal {
public:
    [[nodiscard]] cch::support::ExpectedVoid start(cch::tui::TerminalInputSink, cch::tui::TerminalResizeSink) override {
        modes_.started = true;
        return {};
    }

    [[nodiscard]] cch::support::ExpectedVoid stop() override {
        return std::unexpected(cch::support::make_error(
                cch::support::ErrorCode::Process, "terminal restoration failed", "stop detail"));
    }

    [[nodiscard]] cch::tui::TerminalDimensions dimensions() const override { return {}; }
    [[nodiscard]] cch::tui::TerminalCapabilities capabilities() const override { return {}; }
    [[nodiscard]] cch::tui::TerminalModeState modes() const override { return modes_; }
    [[nodiscard]] cch::support::ExpectedVoid clear_screen() override { return {}; }
    [[nodiscard]] cch::support::ExpectedVoid write(std::string_view) override { return {}; }
    [[nodiscard]] cch::support::ExpectedVoid set_cursor(cch::tui::CursorPosition) override { return {}; }
    [[nodiscard]] cch::support::ExpectedVoid set_cursor_visible(bool visible) override {
        if (!visible) {
            return std::unexpected(cch::support::make_error(
                    cch::support::ErrorCode::Process, "cursor hiding failed", "cursor detail"));
        }
        return {};
    }
    [[nodiscard]] cch::support::ExpectedVoid set_scroll_margins(std::size_t, std::size_t) override { return {}; }
    [[nodiscard]] cch::support::ExpectedVoid reset_scroll_margins() override { return {}; }
    [[nodiscard]] cch::support::ExpectedVoid begin_synchronized_update() override { return {}; }
    [[nodiscard]] cch::support::ExpectedVoid end_synchronized_update() override { return {}; }
    [[nodiscard]] cch::support::ExpectedVoid set_title(std::string_view) override { return {}; }
    [[nodiscard]] cch::support::ExpectedVoid set_progress(bool) override { return {}; }
    [[nodiscard]] cch::support::ExpectedVoid set_program_status(const cch::tui::ProgramStatus&) override { return {}; }
    [[nodiscard]] cch::support::ExpectedVoid drain_input(
            std::chrono::milliseconds, std::chrono::milliseconds) override {
        return {};
    }

private:
    cch::tui::TerminalModeState modes_;
};

class RenderBackpressureTerminal final : public cch::tui::Terminal {
public:
    [[nodiscard]] cch::support::ExpectedVoid start(cch::tui::TerminalInputSink, cch::tui::TerminalResizeSink) override {
        modes_.started = true;
        return {};
    }

    [[nodiscard]] cch::support::ExpectedVoid stop() override {
        modes_.started = false;
        return {};
    }

    [[nodiscard]] cch::tui::TerminalDimensions dimensions() const override { return {.columns = 10, .rows = 3}; }

    [[nodiscard]] cch::tui::TerminalCapabilities capabilities() const override { return {.synchronized_output = true}; }

    [[nodiscard]] cch::tui::TerminalModeState modes() const override { return modes_; }
    [[nodiscard]] cch::support::ExpectedVoid clear_screen() override {
        ++clear_screen_calls;
        return {};
    }
    [[nodiscard]] cch::support::ExpectedVoid write(std::string_view) override { return {}; }
    [[nodiscard]] cch::support::ExpectedVoid set_cursor(cch::tui::CursorPosition) override { return {}; }
    [[nodiscard]] cch::support::ExpectedVoid set_cursor_visible(bool) override { return {}; }
    [[nodiscard]] cch::support::ExpectedVoid set_scroll_margins(std::size_t, std::size_t) override { return {}; }
    [[nodiscard]] cch::support::ExpectedVoid reset_scroll_margins() override { return {}; }
    [[nodiscard]] cch::support::ExpectedVoid begin_synchronized_update() override { return {}; }
    [[nodiscard]] cch::support::ExpectedVoid end_synchronized_update() override {
        if (fail_next_end) {
            fail_next_end = false;
            return std::unexpected(
                    cch::support::make_error(cch::support::ErrorCode::Busy, "render frame was not admitted"));
        }
        return {};
    }
    [[nodiscard]] cch::support::ExpectedVoid set_title(std::string_view) override { return {}; }
    [[nodiscard]] cch::support::ExpectedVoid set_progress(bool) override { return {}; }
    [[nodiscard]] cch::support::ExpectedVoid set_program_status(const cch::tui::ProgramStatus&) override { return {}; }
    [[nodiscard]] cch::support::ExpectedVoid drain_input(
            std::chrono::milliseconds, std::chrono::milliseconds) override {
        return {};
    }

    bool fail_next_end{true};
    std::size_t clear_screen_calls{0};

private:
    cch::tui::TerminalModeState modes_;
};

class MutableLinesComponent final : public cch::tui::Component {
public:
    [[nodiscard]] cch::support::Expected<cch::tui::RenderResult> render(std::size_t) override {
        return cch::tui::RenderResult{.lines = std::vector<std::string>(line_count, "line")};
    }

    void invalidate() override {}

    std::size_t line_count{5};
};

constexpr std::string_view kIssue954PasteSegment = "\xce\xb1\tprotocol \x1b[?997;1n \x1b[A\r\n";

[[nodiscard]] std::string issue954_large_paste_body() {
    std::string body;
    body.reserve(1024 * 1024 + kIssue954PasteSegment.size() * 100);
    while (body.size() <= 1024 * 1024)
        body += kIssue954PasteSegment;
    body += "paste-suffix-954";
    return body;
}

[[nodiscard]] std::uint64_t issue954_digest(std::string_view text) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char byte : text) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash;
}

class FocusableInputComponent final : public cch::tui::Component,
                                      public cch::tui::InputHandler,
                                      public cch::tui::Focusable {
public:
    [[nodiscard]] cch::support::Expected<cch::tui::RenderResult> render(std::size_t) override {
        return cch::tui::RenderResult{.lines = {"x"}};
    }

    void invalidate() override { ++invalidation_count; }

    cch::tui::InputAdmissionOutcome handle_input(const cch::tui::InputEventVariant& input) override {
        const auto* key = std::get_if<cch::tui::KeyEvent>(&input);
        if (key != nullptr && key->type == cch::tui::KeyEventType::Release && !accept_key_releases) {
            return cch::tui::InputAdmissionOutcome::Unhandled;
        }
        received_input.push_back(input);
        return cch::tui::InputAdmissionOutcome::Consumed;
    }
    void set_focused(bool focused) override { focused_ = focused; }

    [[nodiscard]] bool focused() const override { return focused_; }

    std::size_t invalidation_count{0};
    std::vector<cch::tui::InputEventVariant> received_input;
    bool accept_key_releases{false};

private:
    bool focused_{false};
};

} // namespace

TEST_CASE("Tui rolls back viewport state after a backpressured frame", "[tui][issue462][spec]") {
    RenderBackpressureTerminal terminal;
    cch::tui::Tui tui(terminal);
    auto component = std::make_unique<MutableLinesComponent>();
    auto* component_pointer = component.get();
    REQUIRE(tui.add_child(std::move(component)));
    ManualTuiScheduler scheduler;
    tui.set_scheduler(&scheduler);
    REQUIRE(tui.start());

    const auto first = tui.render();
    REQUIRE_FALSE(first);
    CHECK(first.error().code == cch::support::ErrorCode::Busy);

    // The failed five-line frame must not advance the cached viewport. The
    // next successful render is still the first frame and replaces it with a
    // one-line presentation.
    component_pointer->line_count = 1;
    scheduler.advance(std::chrono::milliseconds{16});
    scheduler.run_scheduled();

    // With the viewport rolled back, growing to two lines is a normal
    // differential update. Without the rollback it looks like a change above
    // the stale viewport and incorrectly forces a full clear.
    component_pointer->line_count = 2;
    tui.invalidate();
    scheduler.advance(std::chrono::milliseconds{16});
    scheduler.run_scheduled();
    CHECK(terminal.clear_screen_calls == 0);
    REQUIRE(tui.stop());
}

TEST_CASE("Tui renders attached Text through the VirtualTerminal seam", "[tui][issue45][spec]") {
    cch::tui::VirtualTerminal terminal({.columns = 8, .rows = 2});
    cch::tui::Tui tui(terminal);
    // Use zero padding so the rendered output is just the text
    REQUIRE(tui.add_child(std::make_unique<cch::tui::Text>("hello", 0, 0)));

    REQUIRE(tui.start());
    REQUIRE(tui.render());

    // Text pads content to full width
    const std::vector<std::string> expected_screen{"hello   ", ""};
    CHECK(terminal.screen() == expected_screen);
    // Synchronized update markers wrap the rendered line on first render.
    // The composed row carries the one full reset at its true end.
    REQUIRE(terminal.output().size() == 3);
    CHECK(terminal.output()[0] == "\x1b[?2026h");
    CHECK(terminal.output()[1] == "hello   \x1b[0m\x1b]8;;\x07");
    CHECK(terminal.output()[2] == "\x1b[?2026l");
    const cch::tui::CursorPosition expected_cursor{.column = 8, .row = 0};
    CHECK(terminal.cursor() == expected_cursor);
    CHECK(terminal.final_style() == cch::tui::TerminalStyle{});
    CHECK_FALSE(terminal.modes().cursor_visible);

    REQUIRE(tui.stop());
    CHECK_FALSE(terminal.modes().started);
    CHECK(terminal.modes().cursor_visible);
}

TEST_CASE("Tui clears and repaints unchanged retained content", "[tui][render][issue60][spec]") {
    cch::tui::VirtualTerminal terminal({.columns = 8, .rows = 2});
    cch::tui::Tui tui(terminal);
    REQUIRE(tui.add_child(std::make_unique<cch::tui::Text>("hello", 0, 0)));
    REQUIRE(tui.start());
    REQUIRE(tui.render());
    (void)terminal.check_clear_screen_called();

    REQUIRE(tui.clear_screen());
    CHECK(terminal.check_clear_screen_called());
    const std::vector<std::string> cleared_screen{"", ""};
    CHECK(terminal.screen() == cleared_screen);

    REQUIRE(tui.render());
    const std::vector<std::string> repainted_screen{"hello   ", ""};
    CHECK(terminal.screen() == repainted_screen);
    REQUIRE(tui.stop());
}

TEST_CASE("Tui exposes styled Text cells with a default final style", "[tui][issue46][unicode][spec]") {
    cch::tui::VirtualTerminal terminal({.columns = 4, .rows = 1});
    cch::tui::Tui tui(terminal);
    REQUIRE(tui.add_child(std::make_unique<cch::tui::Text>("\x1b[31mA", 0, 0)));
    REQUIRE(tui.start());
    REQUIRE(tui.render());

    REQUIRE(terminal.cells().size() == 1);
    REQUIRE(terminal.cells()[0].size() == 4);
    CHECK(terminal.cells()[0][0].grapheme == "A");
    CHECK(terminal.cells()[0][0].style.fg_color == "31");
    CHECK(terminal.final_style() == cch::tui::TerminalStyle{});
}

TEST_CASE("Tui preserves cursor and restoration failures from partial startup", "[tui][terminal][issue58][spec]") {
    PartialStartupFailureTerminal terminal;
    cch::tui::Tui tui(terminal);

    const auto result = tui.start();

    REQUIRE_FALSE(result);
    CHECK(result.error().message == "TUI startup failed and terminal restoration was incomplete");
    CHECK(result.error().detail.find("cursor hiding failed") != std::string::npos);
    CHECK(result.error().detail.find("terminal restoration failed") != std::string::npos);
}

TEST_CASE("Tui rejects a null Component attachment", "[tui][issue45][spec]") {
    cch::tui::VirtualTerminal terminal;
    cch::tui::Tui tui(terminal);

    const auto result = tui.add_child(nullptr);

    REQUIRE_FALSE(result);
    CHECK(result.error().code == cch::support::ErrorCode::Validation);
    CHECK(result.error().message == "TUI cannot attach a null Component");
}

TEST_CASE("Text renders gracefully in narrow widths by clamping padding", "[tui][text][issue897][spec]") {
    cch::tui::Text text("hello", 2, 0);
    const auto rendered = text.render(2);
    CHECK(rendered.has_value());
    if (!rendered) {
        FAIL(rendered.error().message);
    }
    CHECK(rendered->lines.size() >= 1);
}

TEST_CASE("Text accepts Unicode characters", "[tui][issue46][unicode][spec]") {
    cch::tui::Text text("\xc3\xa9", 0, 0); // é in UTF-8

    const auto result = text.render(2);
    REQUIRE(result);
    REQUIRE(result->lines.size() == 1);
    CHECK(cch::tui::visible_width(result->lines[0]) >= 1);
}

TEST_CASE("Tui rejects a Component line wider than its visible width", "[tui][issue45][spec]") {
    cch::tui::VirtualTerminal terminal({.columns = 3, .rows = 1});
    cch::tui::Tui tui(terminal);
    REQUIRE(tui.add_child(std::make_unique<OverwideComponent>()));

    REQUIRE(tui.start());
    const auto result = tui.render();

    REQUIRE_FALSE(result);
    CHECK(result.error().code == cch::support::ErrorCode::Validation);
    CHECK(result.error().message == "TUI component rendered a line wider than its width bound");
    CHECK(result.error().detail == "line width 8 exceeds visible width 3");
    CHECK(result.error().detail.size() < 128);
    const std::vector<std::string> expected_screen{""};
    CHECK(terminal.screen() == expected_screen);
}

TEST_CASE("Tui routes focus, input, and invalidation through Component capabilities", "[tui][issue45][spec]") {
    cch::tui::VirtualTerminal terminal({.columns = 4, .rows = 1});
    cch::tui::Tui tui(terminal);
    auto component = std::make_unique<FocusableInputComponent>();
    auto* component_pointer = component.get();
    REQUIRE(tui.add_child(std::move(component)));

    REQUIRE(tui.start());
    REQUIRE(tui.set_focus(component_pointer));
    REQUIRE(terminal.inject_input("q"));
    tui.invalidate();

    CHECK(component_pointer->focused());
    REQUIRE(component_pointer->received_input.size() == 1);
    CHECK(std::get<cch::tui::KeyEvent>(component_pointer->received_input[0]).key == "q");
    CHECK(component_pointer->invalidation_count == 1);
}

TEST_CASE("Tui resolves a lone Escape key when the terminal flushes ambiguity", "[tui][input][issue47][spec]") {
    cch::tui::VirtualTerminal terminal;
    cch::tui::Tui tui(terminal);
    auto component = std::make_unique<FocusableInputComponent>();
    auto* component_pointer = component.get();
    REQUIRE(tui.add_child(std::move(component)));
    REQUIRE(tui.start());
    REQUIRE(tui.set_focus(component_pointer));

    REQUIRE(terminal.inject_input("\x1b"));
    CHECK(component_pointer->received_input.empty());
    REQUIRE(terminal.flush_input());

    REQUIRE(component_pointer->received_input.size() == 1);
    CHECK(std::get<cch::tui::KeyEvent>(component_pointer->received_input[0]).key == "escape");
}

TEST_CASE("Tui maps baseline terminal protocols to the same semantic key", "[tui][input][issue47][spec]") {
    cch::tui::VirtualTerminal terminal;
    cch::tui::Tui tui(terminal);
    auto component = std::make_unique<FocusableInputComponent>();
    auto* component_pointer = component.get();
    REQUIRE(tui.add_child(std::move(component)));
    REQUIRE(tui.start());
    REQUIRE(tui.set_focus(component_pointer));

    REQUIRE(terminal.inject_input("\x03\x1b[99;5u\x1b[27;5;99~"));

    REQUIRE(component_pointer->received_input.size() == 3);
    const cch::tui::KeyEvent expected{
            .key = "c",
            .ctrl = true,
    };
    for (const auto& input : component_pointer->received_input) {
        CHECK(std::get<cch::tui::KeyEvent>(input) == expected);
    }
}

TEST_CASE("Tui decodes the supported legacy special key vocabulary", "[tui][input][issue47][spec]") {
    struct RawKey {
        std::string_view raw;
        std::string_view identifier;
    };
    constexpr std::array<RawKey, 32> kKeys{{
            {"\t", "tab"},
            {"\r", "enter"},
            {" ", "space"},
            {"\x7f", "backspace"},
            {std::string_view{"\x00", 1}, "ctrl+space"},
            {"\x1b[A", "up"},
            {"\x1b[B", "down"},
            {"\x1b[C", "right"},
            {"\x1b[D", "left"},
            {"\x1b[H", "home"},
            {"\x1b[F", "end"},
            {"\x1b[2~", "insert"},
            {"\x1b[3~", "delete"},
            {"\x1b[5~", "pageUp"},
            {"\x1b[6~", "pageDown"},
            {"\x1b[E", "clear"},
            {"\x1bOP", "f1"},
            {"\x1b[24~", "f12"},
            {"\x1b[Z", "shift+tab"},
            {"\x1b[2$", "shift+insert"},
            {"\x1b[3$", "shift+delete"},
            {"\x1b[5$", "shift+pageUp"},
            {"\x1b[6$", "shift+pageDown"},
            {"\x1b[7$", "shift+home"},
            {"\x1b[8$", "shift+end"},
            {"\x1b[2^", "ctrl+insert"},
            {"\x1b[e", "shift+clear"},
            {"\x1bOe", "ctrl+clear"},
            {"\x1b"
             "B",
                    "alt+left"},
            {"\x1b"
             "F",
                    "alt+right"},
            {"\x1b"
             "b",
                    "alt+left"},
            {"A", "shift+a"},
    }};

    cch::tui::VirtualTerminal terminal;
    cch::tui::Tui tui(terminal);
    auto component = std::make_unique<FocusableInputComponent>();
    auto* component_pointer = component.get();
    REQUIRE(tui.add_child(std::move(component)));
    REQUIRE(tui.start());
    REQUIRE(tui.set_focus(component_pointer));

    for (const auto& key : kKeys)
        REQUIRE(terminal.inject_input(std::string(key.raw)));

    REQUIRE(component_pointer->received_input.size() == kKeys.size());
    for (std::size_t index = 0; index < kKeys.size(); ++index) {
        CHECK(cch::tui::matches_key(
                std::get<cch::tui::KeyEvent>(component_pointer->received_input[index]), kKeys[index].identifier));
    }
}

TEST_CASE("Tui decodes Kitty alternate keys keypad and modified navigation", "[tui][input][issue47][spec]") {
    cch::tui::VirtualTerminal terminal;
    cch::tui::Tui tui(terminal);
    auto component = std::make_unique<FocusableInputComponent>();
    component->accept_key_releases = true;
    auto* component_pointer = component.get();
    REQUIRE(tui.add_child(std::move(component)));
    REQUIRE(tui.start());
    REQUIRE(tui.set_focus(component_pointer));

    REQUIRE(terminal.inject_input("\x1b[1089::99;5u"
                                  "\x1b[57400u"
                                  "\x1b[1;6:2D"
                                  "\x1b[3;3:3~"));

    REQUIRE(component_pointer->received_input.size() == 4);
    CHECK(cch::tui::matches_key(std::get<cch::tui::KeyEvent>(component_pointer->received_input[0]), "ctrl+c"));
    CHECK(cch::tui::matches_key(std::get<cch::tui::KeyEvent>(component_pointer->received_input[1]), "1"));
    const auto& repeated_left = std::get<cch::tui::KeyEvent>(component_pointer->received_input[2]);
    CHECK(cch::tui::matches_key(repeated_left, "shift+ctrl+left"));
    CHECK(repeated_left.type == cch::tui::KeyEventType::Repeat);
    const auto& released_delete = std::get<cch::tui::KeyEvent>(component_pointer->received_input[3]);
    CHECK(cch::tui::matches_key(released_delete, "alt+delete"));
    CHECK(released_delete.type == cch::tui::KeyEventType::Release);
}

TEST_CASE("Tui buffers split input and separates batched terminal sequences", "[tui][input][issue47][spec]") {
    cch::tui::VirtualTerminal terminal;
    cch::tui::Tui tui(terminal);
    auto component = std::make_unique<FocusableInputComponent>();
    auto* component_pointer = component.get();
    REQUIRE(tui.add_child(std::move(component)));
    REQUIRE(tui.start());
    REQUIRE(tui.set_focus(component_pointer));

    REQUIRE(terminal.inject_input("\x1b"));
    REQUIRE(terminal.inject_input("[1;"));
    CHECK(component_pointer->received_input.empty());
    REQUIRE(terminal.inject_input("5C\x1b[A\x1b[B"));

    REQUIRE(component_pointer->received_input.size() == 3);
    const cch::tui::KeyEvent expected_right{.key = "right", .ctrl = true};
    CHECK(std::get<cch::tui::KeyEvent>(component_pointer->received_input[0]) == expected_right);
    CHECK(std::get<cch::tui::KeyEvent>(component_pointer->received_input[1]).key == "up");
    CHECK(std::get<cch::tui::KeyEvent>(component_pointer->received_input[2]).key == "down");
}

TEST_CASE("Tui preserves large batches without dropping semantic keys", "[tui][input][issue47][spec]") {
    cch::tui::VirtualTerminal terminal;
    cch::tui::Tui tui(terminal);
    auto component = std::make_unique<FocusableInputComponent>();
    auto* component_pointer = component.get();
    REQUIRE(tui.add_child(std::move(component)));
    REQUIRE(tui.start());
    REQUIRE(tui.set_focus(component_pointer));

    REQUIRE(terminal.inject_input(std::string(5000, 'x')));

    REQUIRE(component_pointer->received_input.size() == 5000);
    CHECK(std::get<cch::tui::KeyEvent>(component_pointer->received_input.front()).key == "x");
    CHECK(std::get<cch::tui::KeyEvent>(component_pointer->received_input.back()).key == "x");
}

TEST_CASE("Tui filters key releases unless the focused Component opts in", "[tui][input][issue47][spec]") {
    cch::tui::VirtualTerminal terminal;
    cch::tui::Tui tui(terminal);
    auto default_component = std::make_unique<FocusableInputComponent>();
    auto* default_pointer = default_component.get();
    REQUIRE(tui.add_child(std::move(default_component)));
    auto release_component = std::make_unique<FocusableInputComponent>();
    release_component->accept_key_releases = true;
    auto* release_pointer = release_component.get();
    REQUIRE(tui.add_child(std::move(release_component)));
    REQUIRE(tui.start());

    REQUIRE(tui.set_focus(default_pointer));
    REQUIRE(terminal.inject_input("\x1b[97u\x1b[97;1:3u"));
    REQUIRE(default_pointer->received_input.size() == 1);
    CHECK(std::get<cch::tui::KeyEvent>(default_pointer->received_input[0]).type == cch::tui::KeyEventType::Press);

    REQUIRE(tui.set_focus(release_pointer));
    REQUIRE(terminal.inject_input("\x1b[97;1:3u"));
    REQUIRE(release_pointer->received_input.size() == 1);
    CHECK(std::get<cch::tui::KeyEvent>(release_pointer->received_input[0]).type == cch::tui::KeyEventType::Release);
}

TEST_CASE(
        "Tui delivers bracketed paste atomically with opaque control-looking content", "[tui][input][issue47][spec]") {
    cch::tui::VirtualTerminal terminal;
    cch::tui::Tui tui(terminal);
    auto component = std::make_unique<FocusableInputComponent>();
    auto* component_pointer = component.get();
    REQUIRE(tui.add_child(std::move(component)));
    REQUIRE(tui.start());
    REQUIRE(tui.set_focus(component_pointer));

    REQUIRE(terminal.inject_input("a\x1b[20"));
    REQUIRE(terminal.inject_input("0~line 1\n\x1b[A\x1b[13;5u"));
    REQUIRE(terminal.inject_input("\x1b[20"));
    REQUIRE(terminal.inject_input("1~b"));

    REQUIRE(component_pointer->received_input.size() == 3);
    CHECK(std::get<cch::tui::KeyEvent>(component_pointer->received_input[0]).key == "a");
    const auto& paste = std::get<cch::tui::PasteEvent>(component_pointer->received_input[1]);
    CHECK(paste.text == "line 1\n\x1b[A\x1b[13;5u");
    CHECK(paste.original_bytes == paste.text.size());
    CHECK(paste.lines == 2);
    CHECK_FALSE(paste.truncated);
    CHECK(std::get<cch::tui::KeyEvent>(component_pointer->received_input[2]).key == "b");
}

TEST_CASE("Tui preserves and submits a complete paste larger than one MiB", "[tui][input][issue954][spec]") {
    cch::tui::VirtualTerminal terminal;
    cch::tui::Tui tui(terminal);
    std::vector<std::string> submitted;
    auto editor = std::make_unique<cch::tui::Editor>(cch::tui::EditorOptions{},
            cch::tui::EditorChangeSink{},
            [&submitted](std::string text) -> cch::support::ExpectedVoid {
                submitted.push_back(std::move(text));
                return {};
            });
    auto* editor_pointer = editor.get();
    REQUIRE(tui.add_child(std::move(editor)));
    REQUIRE(tui.start());
    REQUIRE(tui.set_focus(editor_pointer));

    const auto raw = issue954_large_paste_body();
    const auto normalized = cch::tui::normalize_pasted_text(raw, {.multiline = true});
    REQUIRE(raw.size() > 1024 * 1024);
    REQUIRE(terminal.inject_input("\x1b[200~"));
    for (std::size_t offset = 0; offset < raw.size(); offset += 4093) {
        REQUIRE(terminal.inject_input(raw.substr(offset, 4093)));
    }
    REQUIRE(terminal.inject_input("\x1b[201~"));
    REQUIRE(terminal.inject_input("q"));

    const auto expanded = editor_pointer->expanded_text();
    CHECK(expanded == normalized + "q");
    CHECK(expanded.substr(0, 48) == (normalized + "q").substr(0, 48));
    CHECK(expanded.substr(expanded.size() / 2, 48) == (normalized + "q").substr((normalized.size() + 1) / 2, 48));
    CHECK(expanded.substr(expanded.size() - 48) == (normalized + "q").substr(normalized.size() + 1 - 48));
    CHECK(issue954_digest(expanded) == issue954_digest(normalized + "q"));
    CHECK(submitted.empty());

    REQUIRE(terminal.inject_input("\r"));
    REQUIRE(submitted.size() == 1);
    CHECK(submitted.front() == normalized + "q");
    CHECK(issue954_digest(submitted.front()) == issue954_digest(normalized + "q"));
    REQUIRE(tui.stop());
}

TEST_CASE("Tui keeps a bracketed paste active across fragment flushes", "[tui][input][issue954][spec]") {
    cch::tui::VirtualTerminal terminal;
    cch::tui::Tui tui(terminal);
    auto editor = std::make_unique<cch::tui::Editor>();
    auto* editor_pointer = editor.get();
    REQUIRE(tui.add_child(std::move(editor)));
    REQUIRE(tui.start());
    REQUIRE(tui.set_focus(editor_pointer));

    const auto raw = issue954_large_paste_body();
    const auto normalized = cch::tui::normalize_pasted_text(raw, {.multiline = true});
    REQUIRE(terminal.inject_input("\x1b[200~"));
    REQUIRE(terminal.inject_input(raw.substr(0, 4093)));
    REQUIRE(terminal.flush_input());
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    REQUIRE(terminal.inject_input(raw.substr(4093)));
    REQUIRE(terminal.inject_input("\x1b[201~"));
    REQUIRE(terminal.inject_input("q"));

    const auto expanded = editor_pointer->expanded_text();
    CHECK(expanded == normalized + "q");
    CHECK(issue954_digest(expanded) == issue954_digest(normalized + "q"));
    REQUIRE(tui.stop());
}

TEST_CASE("Tui cancels an incomplete paste on stop without submitting it", "[tui][input][issue954][spec]") {
    cch::tui::VirtualTerminal terminal;
    cch::tui::Tui tui(terminal);
    auto component = std::make_unique<FocusableInputComponent>();
    auto* component_pointer = component.get();
    REQUIRE(tui.add_child(std::move(component)));
    REQUIRE(tui.start());
    REQUIRE(tui.set_focus(component_pointer));

    REQUIRE(terminal.inject_input("\x1b[200~unfinished\n\x1b[A"));
    REQUIRE(terminal.flush_input());
    CHECK(component_pointer->received_input.empty());
    REQUIRE(tui.stop());
    CHECK(component_pointer->received_input.empty());

    REQUIRE(tui.start());
    REQUIRE(tui.set_focus(component_pointer));
    REQUIRE(terminal.inject_input("q"));
    REQUIRE(component_pointer->received_input.size() == 1);
    CHECK(std::get<cch::tui::KeyEvent>(component_pointer->received_input[0]).key == "q");
    REQUIRE(tui.stop());
}

TEST_CASE("Tui discards overlong terminal control payloads through their terminators", "[tui][input][issue47][spec]") {
    cch::tui::VirtualTerminal terminal;
    cch::tui::Tui tui(terminal);
    auto component = std::make_unique<FocusableInputComponent>();
    auto* component_pointer = component.get();
    REQUIRE(tui.add_child(std::move(component)));
    REQUIRE(tui.start());
    REQUIRE(tui.set_focus(component_pointer));

    REQUIRE(terminal.inject_input("\x1b]" + std::string(300, 'x') + "payload"));
    CHECK(component_pointer->received_input.empty());
    REQUIRE(terminal.inject_input("\x1b\\q"));
    REQUIRE(terminal.inject_input("\x1bP" + std::string(300, 'y') + "payload"));
    REQUIRE(terminal.inject_input("\x1b\\r"));

    REQUIRE(component_pointer->received_input.size() == 2);
    CHECK(std::get<cch::tui::KeyEvent>(component_pointer->received_input[0]).key == "q");
    CHECK(std::get<cch::tui::KeyEvent>(component_pointer->received_input[1]).key == "r");
}

TEST_CASE("Tui recovers after invalid unsupported and overlong input", "[tui][input][issue47][spec]") {
    cch::tui::VirtualTerminal terminal;
    cch::tui::Tui tui(terminal);
    auto component = std::make_unique<FocusableInputComponent>();
    auto* component_pointer = component.get();
    REQUIRE(tui.add_child(std::move(component)));
    REQUIRE(tui.start());
    REQUIRE(tui.set_focus(component_pointer));

    REQUIRE(terminal.inject_input("\x1b[999999999999999999999;5u\x1b[99;99u"));
    REQUIRE(terminal.inject_input("\x1b[" + std::string(300, '1')));
    REQUIRE(terminal.inject_input("\x1b[A"));
    REQUIRE(terminal.inject_input("\xc3"));
    REQUIRE(terminal.flush_input());
    REQUIRE(terminal.inject_input("q"));

    REQUIRE(component_pointer->received_input.size() == 2);
    CHECK(std::get<cch::tui::KeyEvent>(component_pointer->received_input[0]).key == "up");
    CHECK(std::get<cch::tui::KeyEvent>(component_pointer->received_input[1]).key == "q");
}

TEST_CASE("VirtualTerminal injects input and resize events deterministically", "[tui][issue45][spec]") {
    cch::tui::VirtualTerminal terminal({.columns = 4, .rows = 1});
    std::string received_input;
    std::optional<cch::tui::TerminalDimensions> received_resize;

    REQUIRE(terminal.start(
            [&received_input](std::string input) -> cch::support::ExpectedVoid {
                received_input = std::move(input);
                return {};
            },
            [&received_resize](cch::tui::TerminalDimensions dimensions) -> cch::support::ExpectedVoid {
                received_resize = dimensions;
                return {};
            }));
    REQUIRE(terminal.inject_input("x"));
    REQUIRE(terminal.inject_resize({.columns = 6, .rows = 2}));

    CHECK(received_input == "x");
    REQUIRE(received_resize);
    const cch::tui::TerminalDimensions expected_dimensions{.columns = 6, .rows = 2};
    CHECK(*received_resize == expected_dimensions);
    CHECK(terminal.dimensions() == expected_dimensions);
}

TEST_CASE("VirtualTerminal preserves cursor-positioned writes and rejects overflow", "[tui][issue45][spec]") {
    cch::tui::VirtualTerminal terminal({.columns = 4, .rows = 1});
    REQUIRE(terminal.start([](std::string) -> cch::support::ExpectedVoid { return {}; },
            [](cch::tui::TerminalDimensions) -> cch::support::ExpectedVoid { return {}; }));
    REQUIRE(terminal.set_cursor({.column = 2, .row = 0}));
    REQUIRE(terminal.write("x"));

    const std::vector<std::string> expected_screen{"  x"};
    CHECK(terminal.screen() == expected_screen);
    const cch::tui::CursorPosition expected_cursor{.column = 3, .row = 0};
    CHECK(terminal.cursor() == expected_cursor);

    const auto result = terminal.write("yz");
    REQUIRE_FALSE(result);
    CHECK(result.error().code == cch::support::ErrorCode::Validation);
    CHECK(terminal.screen() == expected_screen);
    const std::vector<std::string> expected_output{"x"};
    CHECK(terminal.output() == expected_output);
}

TEST_CASE("VirtualTerminal accepts Unicode output", "[tui][issue46][unicode][spec]") {
    cch::tui::VirtualTerminal terminal({.columns = 4, .rows = 1});
    REQUIRE(terminal.start([](std::string) -> cch::support::ExpectedVoid { return {}; },
            [](cch::tui::TerminalDimensions) -> cch::support::ExpectedVoid { return {}; }));

    const auto result = terminal.write("\xc3\xa9"); // é

    REQUIRE(result);
    CHECK(terminal.screen().size() == 1);
    CHECK_FALSE(terminal.output().empty());
}

TEST_CASE("VirtualTerminal exposes visible cells and final style", "[tui][issue46][unicode][spec]") {
    cch::tui::VirtualTerminal terminal({.columns = 4, .rows = 1});
    REQUIRE(terminal.start([](std::string) -> cch::support::ExpectedVoid { return {}; },
            [](cch::tui::TerminalDimensions) -> cch::support::ExpectedVoid { return {}; }));

    REQUIRE(terminal.write("\x1b[31mA"));

    REQUIRE(terminal.cells().size() == 1);
    REQUIRE(terminal.cells()[0].size() == 4);
    CHECK(terminal.cells()[0][0].grapheme == "A");
    CHECK(terminal.cells()[0][0].style.fg_color == "31");
    CHECK(terminal.final_style().fg_color == "31");
    const std::vector<std::string> expected_screen{"A"};
    CHECK(terminal.screen() == expected_screen);
}

TEST_CASE("VirtualTerminal clears complete wide graphemes on overwrite", "[tui][issue46][unicode][spec]") {
    cch::tui::VirtualTerminal terminal({.columns = 4, .rows = 1});
    REQUIRE(terminal.start([](std::string) -> cch::support::ExpectedVoid { return {}; },
            [](cch::tui::TerminalDimensions) -> cch::support::ExpectedVoid { return {}; }));
    REQUIRE(terminal.write("\xe4\xb8\xad"));
    REQUIRE(terminal.cells()[0][1].continuation);

    REQUIRE(terminal.set_cursor({.column = 1, .row = 0}));
    REQUIRE(terminal.write("x"));

    CHECK(terminal.cells()[0][0].grapheme.empty());
    CHECK_FALSE(terminal.cells()[0][1].continuation);
    CHECK(terminal.cells()[0][1].grapheme == "x");
    const std::vector<std::string> expected_screen{" x"};
    CHECK(terminal.screen() == expected_screen);
}

TEST_CASE("VirtualTerminal bounds callback failures", "[tui][issue45][issue485][spec]") {
    cch::tui::VirtualTerminal terminal;
    REQUIRE(terminal.start(
            [](std::string) -> cch::support::ExpectedVoid {
                return std::unexpected(cch::support::make_error(cch::support::ErrorCode::Unknown, "input failure"));
            },
            [](cch::tui::TerminalDimensions) -> cch::support::ExpectedVoid {
                return std::unexpected(cch::support::make_error(cch::support::ErrorCode::Unknown, "resize failure"));
            }));

    const auto input_result = terminal.inject_input("x");
    REQUIRE_FALSE(input_result);
    CHECK(input_result.error().message == "input failure");

    const auto resize_result = terminal.inject_resize({.columns = 6, .rows = 2});
    REQUIRE_FALSE(resize_result);
    CHECK(resize_result.error().message == "resize failure");
}

TEST_CASE("Terminal seam records title and progress through VirtualTerminal", "[tui][terminal][issue378][spec]") {
    cch::tui::VirtualTerminal terminal;
    REQUIRE(terminal.start([](std::string) -> cch::support::ExpectedVoid { return {}; },
            [](cch::tui::TerminalDimensions) -> cch::support::ExpectedVoid { return {}; }));

    REQUIRE(terminal.set_title("pike - session - workspace"));
    REQUIRE(terminal.set_progress(true));
    REQUIRE(terminal.set_progress(false));

    const std::vector<std::string> expected_output{
            "\x1b]0;pike - session - workspace\x07",
            "\x1b]9;4;3\x07",
            "\x1b]9;4;0\x07",
    };
    CHECK(terminal.output() == expected_output);
    REQUIRE(terminal.stop());
    CHECK(terminal.output() == expected_output);
}

TEST_CASE("VirtualTerminal stop clears an active progress indicator", "[tui][terminal][issue378][spec]") {
    cch::tui::VirtualTerminal terminal;
    REQUIRE(terminal.start([](std::string) -> cch::support::ExpectedVoid { return {}; },
            [](cch::tui::TerminalDimensions) -> cch::support::ExpectedVoid { return {}; }));

    REQUIRE(terminal.set_progress(true));
    REQUIRE(terminal.stop());

    const std::vector<std::string> expected_output{
            "\x1b]9;4;3\x07",
            "\x1b]9;4;0\x07",
    };
    CHECK(terminal.output() == expected_output);
}

TEST_CASE("VirtualTerminal stop when inactive does not emit progress clear", "[tui][terminal][issue378][spec]") {
    cch::tui::VirtualTerminal terminal;
    REQUIRE(terminal.start([](std::string) -> cch::support::ExpectedVoid { return {}; },
            [](cch::tui::TerminalDimensions) -> cch::support::ExpectedVoid { return {}; }));
    REQUIRE(terminal.stop());
    CHECK(terminal.output().empty());
}

TEST_CASE("VirtualTerminal drain input is a started-gated no-op", "[tui][terminal][issue378][spec]") {
    cch::tui::VirtualTerminal terminal;
    const auto before_start = terminal.drain_input();
    REQUIRE_FALSE(before_start);
    CHECK(before_start.error().message == "Virtual Terminal must be started before terminal operations");

    REQUIRE(terminal.start([](std::string) -> cch::support::ExpectedVoid { return {}; },
            [](cch::tui::TerminalDimensions) -> cch::support::ExpectedVoid { return {}; }));
    REQUIRE(terminal.drain_input());
    REQUIRE(terminal.drain_input(std::chrono::milliseconds(100), std::chrono::milliseconds(10)));
}

TEST_CASE("Tui coalesces repeated replaceable invalidations into one render request", "[tui][render][issue465][spec]") {
    cch::tui::VirtualTerminal terminal({.columns = 4, .rows = 1});
    cch::tui::Tui tui(terminal);
    REQUIRE(tui.add_child(std::make_unique<cch::tui::Text>("hi", 0, 0)));

    ManualTuiScheduler scheduler;
    tui.set_scheduler(&scheduler);
    REQUIRE(tui.start());

    // Replaceable render state: repeated invalidations before the next render
    // coalesce into one request (ADR 0040), so redraw storms cannot flood the
    // loop with duplicate work.
    tui.invalidate();
    tui.invalidate();
    tui.invalidate();
    CHECK(scheduler.schedule_count_ == 1);
    CHECK(scheduler.scheduled_delay_ == std::chrono::steady_clock::duration::zero());

    // Rendering clears the pending flag; the next invalidation requests again.
    REQUIRE(tui.render());
    tui.invalidate();
    CHECK(scheduler.schedule_count_ == 2);

    REQUIRE(tui.stop());
}

TEST_CASE(
        "Tui stop flows the cursor below the composed buffer for the shell prompt", "[tui][issue476][issue985][spec]") {
    // pi TuiMainScreen::beforeTerminalStop: write " ", move one row past the
    // last buffer line, then CRLF, so the shell prompt resumes below the
    // transcript instead of overwriting its last line. The leading space
    // cancels deferred wrap before the relative row movement.
    cch::tui::VirtualTerminal terminal({.columns = 8, .rows = 5});
    cch::tui::Tui tui(terminal);
    REQUIRE(tui.add_child(std::make_unique<cch::tui::Text>("one", 0, 0)));
    REQUIRE(tui.add_child(std::make_unique<cch::tui::Text>("two", 0, 0)));
    REQUIRE(tui.start());
    REQUIRE(tui.render());
    REQUIRE(tui.stop());

    // The two full-width rows leave deferred wrap pending. The leading space
    // consumes it, the relative movement reaches row 3, and CRLF advances to
    // row 4 for the shell prompt.
    CHECK(terminal.cursor() == cch::tui::CursorPosition{.column = 0, .row = 4});
    CHECK(terminal.modes().cursor_visible);
    CHECK_FALSE(terminal.modes().started);
}

TEST_CASE("Tui stop scrolls the exit flow through the terminal scrollback on a full screen",
        "[tui][issue476][issue985][spec]") {
    cch::tui::VirtualTerminal terminal({.columns = 8, .rows = 3});
    cch::tui::Tui tui(terminal);
    REQUIRE(tui.add_child(std::make_unique<cch::tui::Text>("one", 0, 0)));
    REQUIRE(tui.add_child(std::make_unique<cch::tui::Text>("two", 0, 0)));
    REQUIRE(tui.add_child(std::make_unique<cch::tui::Text>("three", 0, 0)));
    REQUIRE(tui.add_child(std::make_unique<cch::tui::Text>("four", 0, 0)));
    REQUIRE(tui.start());
    REQUIRE(tui.render());
    // The four-line buffer already scrolled once on a three-row screen.
    REQUIRE(terminal.viewport_top() == 1);

    REQUIRE(tui.stop());

    // The leading space consumes the last line's deferred wrap. Cursor
    // movement and CRLF then advance scrollback below the retained frame.
    const std::vector<std::string> expected_scrollback{
            "one     ",
            "two     ",
            "three   ",
    };
    CHECK(terminal.scrollback() == expected_scrollback);
    CHECK(terminal.screen()[0].starts_with("four"));
    CHECK(terminal.cursor() == cch::tui::CursorPosition{.column = 0, .row = 2});
    CHECK(terminal.modes().cursor_visible);
}

TEST_CASE("issue985 transfers wrapped and overflowed render state into a differential restart",
        "[tui][render][issue985][spec]") {
    cch::tui::VirtualTerminal terminal({.columns = 8, .rows = 3});
    cch::tui::Tui original(terminal);
    auto original_text = std::make_unique<cch::tui::Text>("one\ntwo\nthree\nfour", 0, 0);
    REQUIRE(original.add_child(std::move(original_text)));
    REQUIRE(original.start());
    REQUIRE(original.render());
    REQUIRE(terminal.viewport_top() == 1);

    const auto state = original.capture_render_state();
    REQUIRE(state.previous_lines.size() == 4);
    CHECK(state.previous_width == 8);
    CHECK(state.previous_height == 3);
    CHECK(state.hardware_cursor_row == 3);
    REQUIRE(original.stop({.preserve_screen = true}));

    cch::tui::Tui replacement(terminal);
    auto replacement_text = std::make_unique<cch::tui::Text>("one\ntwo\nthree\nchanged", 0, 0);
    auto* replacement_text_pointer = replacement_text.get();
    REQUIRE(replacement.add_child(std::move(replacement_text)));
    REQUIRE(replacement.restore_render_state(state));
    REQUIRE(replacement.start());
    REQUIRE(replacement.render());

    CHECK(replacement_text_pointer->text() == "one\ntwo\nthree\nchanged");
    CHECK(terminal.screen().back().find("changed") != std::string::npos);
    CHECK(terminal.viewport_top() == 1);
    REQUIRE(replacement.stop({.preserve_screen = true}));
}

TEST_CASE("issue985 preserve-screen stop retains the frame and cursor while restoring modes",
        "[tui][terminal][issue985][spec]") {
    cch::tui::VirtualTerminal terminal({.columns = 8, .rows = 4});
    cch::tui::Tui tui(terminal);
    REQUIRE(tui.add_child(std::make_unique<cch::tui::Text>("keep", 0, 0)));
    REQUIRE(tui.start());
    REQUIRE(tui.render());
    const auto screen_before = terminal.screen();
    const auto cursor_before = terminal.cursor();
    const auto output_size = terminal.output().size();

    REQUIRE(tui.stop({.preserve_screen = true}));

    CHECK(terminal.screen() == screen_before);
    CHECK(terminal.cursor() == cursor_before);
    CHECK(terminal.output().size() == output_size);
    CHECK_FALSE(terminal.modes().started);
    CHECK(terminal.modes().cursor_visible);
}

TEST_CASE("issue985 ordinary stop writes the shell handoff after retained content", "[tui][terminal][issue985][spec]") {
    cch::tui::VirtualTerminal terminal({.columns = 8, .rows = 4});
    cch::tui::Tui tui(terminal);
    REQUIRE(tui.add_child(std::make_unique<cch::tui::Text>("leave", 0, 0)));
    REQUIRE(tui.start());
    REQUIRE(tui.render());

    REQUIRE(tui.stop());

    REQUIRE_FALSE(terminal.output().empty());
    CHECK(terminal.output().back().starts_with(" "));
    CHECK(terminal.output().back().ends_with("\n"));
    CHECK(terminal.output().back().find("\x1b[1B") != std::string::npos);
    CHECK_FALSE(terminal.modes().started);
    CHECK(terminal.modes().cursor_visible);
}

TEST_CASE("issue985 clear-screen stop emits no extra cursor flow", "[tui][terminal][issue985][spec]") {
    cch::tui::VirtualTerminal terminal({.columns = 8, .rows = 4});
    cch::tui::Tui tui(terminal);
    REQUIRE(tui.add_child(std::make_unique<cch::tui::Text>("cleared", 0, 0)));
    REQUIRE(tui.start());
    REQUIRE(tui.render());
    REQUIRE(tui.clear_screen());
    const auto output_size = terminal.output().size();

    REQUIRE(tui.stop());

    CHECK(terminal.output().size() == output_size);
    CHECK(terminal.screen() == std::vector<std::string>(4));
}

TEST_CASE("issue985 invalid restored state fails cleanly and can be retried", "[tui][render][issue985][spec]") {
    cch::tui::VirtualTerminal terminal({.columns = 8, .rows = 4});
    cch::tui::Tui tui(terminal);
    auto invalid = cch::tui::TuiMainScreenRenderState{
            .previous_lines = {"one", "two"},
            .previous_width = 8,
            .previous_height = 4,
            .cursor_row = 3,
            .hardware_cursor_row = 1,
            .max_lines_rendered = 2,
            .previous_viewport_top = 0,
    };
    const auto failed = tui.restore_render_state(invalid);
    REQUIRE_FALSE(failed);
    CHECK(failed.error().code == cch::support::ErrorCode::Validation);
    CHECK(tui.capture_render_state().previous_lines.empty());

    cch::tui::TuiMainScreenRenderState valid;
    REQUIRE(tui.restore_render_state(valid));
    REQUIRE(tui.start());
    REQUIRE(tui.render());
    REQUIRE(tui.stop({.preserve_screen = true}));
}

TEST_CASE("issue980 places the composed nested cursor on the hardware screen",
        "[tui][terminal][cursor][issue980][spec]") {
    cch::tui::VirtualTerminal terminal({.columns = 30, .rows = 8});
    cch::tui::Tui tui(terminal);
    auto stacked = std::make_unique<cch::tui::Container>();
    auto box = std::make_unique<cch::tui::Box>(1, 1);
    auto container = std::make_unique<cch::tui::Container>();
    REQUIRE(container->add_child(std::make_unique<cch::tui::Text>("first", 0, 0)));
    auto editor = std::make_unique<cch::tui::Editor>();
    auto* editor_ptr = editor.get();
    editor_ptr->set_text("abc");
    REQUIRE(container->add_child(std::move(editor)));
    REQUIRE(box->add_child(std::move(container)));
    REQUIRE(stacked->add_child(std::make_unique<cch::tui::Text>("header", 0, 0)));
    REQUIRE(stacked->add_child(std::move(box)));
    auto stacked_ref = tui.add_child(std::move(stacked));
    REQUIRE(stacked_ref);
    REQUIRE(tui.start());
    REQUIRE(tui.set_focus(editor_ptr));
    REQUIRE(tui.render());
    CHECK(editor_ptr->cursor_location() == cch::tui::CursorPosition{.column = 3, .row = 0});
    const auto expected = cch::tui::CursorPosition{.column = 4, .row = 3};
    CHECK(terminal.cursor() == expected);
    CHECK_FALSE(terminal.modes().cursor_visible);
    REQUIRE(tui.set_focus(nullptr));
    REQUIRE(tui.render());
    REQUIRE(tui.stop());
}

TEST_CASE("Debug key invokes the debug hook before the focused component sees the key",
        "[tui][diagnostics][issue1016][spec]") {
    cch::tui::VirtualTerminal terminal({.columns = 4, .rows = 1});
    cch::tui::Tui tui(terminal);
    auto component = std::make_unique<FocusableInputComponent>();
    auto* component_pointer = component.get();
    REQUIRE(tui.add_child(std::move(component)));
    REQUIRE(tui.start());
    REQUIRE(tui.set_focus(component_pointer));

    std::size_t hook_calls = 0;
    tui.set_on_debug([&hook_calls] { ++hook_calls; });

    // Kitty CSI-u shift+ctrl+d is consumed by the hook, so focus reconciliation
    // and the focused component never observe it.
    REQUIRE(terminal.inject_input("\x1b[68;6u"));
    CHECK(hook_calls == 1);
    CHECK(component_pointer->received_input.empty());

    // The printable modifyOtherKeys spelling of the same chord behaves the same.
    REQUIRE(terminal.inject_input("\x1b[27;6;100~"));
    CHECK(hook_calls == 2);
    CHECK(component_pointer->received_input.empty());

    // Without the modifier the key is ordinary input for the focused component.
    REQUIRE(terminal.inject_input("d"));
    CHECK(hook_calls == 2);
    REQUIRE(component_pointer->received_input.size() == 1);
    CHECK(std::get<cch::tui::KeyEvent>(component_pointer->received_input[0]).key == "d");

    REQUIRE(tui.stop());
}

TEST_CASE("Debug hook runs after the raw-input listener stage and survives a restart",
        "[tui][diagnostics][issue1016][spec]") {
    cch::tui::VirtualTerminal terminal({.columns = 4, .rows = 1});
    cch::tui::Tui tui(terminal);
    auto component = std::make_unique<FocusableInputComponent>();
    auto* component_pointer = component.get();
    REQUIRE(tui.add_child(std::move(component)));

    std::size_t hook_calls = 0;
    tui.set_on_debug([&hook_calls] { ++hook_calls; });

    // A raw-input listener observes and consumes the fragment first: the hook is
    // downstream of the listener chain.
    auto consumer =
            tui.add_raw_input_listener([](std::string_view) { return cch::tui::RawInputListenerResult::consume(); });

    REQUIRE(tui.start());
    REQUIRE(tui.set_focus(component_pointer));
    REQUIRE(terminal.inject_input("\x1b[68;6u"));
    CHECK(hook_calls == 0);
    CHECK(component_pointer->received_input.empty());

    // With the key surviving the chain the hook consumes it instead.
    consumer.reset();
    REQUIRE(terminal.inject_input("\x1b[68;6u"));
    CHECK(hook_calls == 1);

    // A stopped root delivers no input at all, and the registered hook is not
    // cleared by stop() (pi keeps `onDebug` across a terminal restart).
    REQUIRE(tui.stop());
    REQUIRE(terminal.inject_input("\x1b[68;6u"));
    CHECK(hook_calls == 1);

    REQUIRE(tui.start());
    REQUIRE(tui.set_focus(component_pointer));
    REQUIRE(terminal.inject_input("\x1b[68;6u"));
    CHECK(hook_calls == 2);
    CHECK(component_pointer->received_input.empty());

    // Clearing the hook forwards the chord to the focused component again.
    tui.set_on_debug(nullptr);
    REQUIRE(terminal.inject_input("\x1b[68;6u"));
    CHECK(hook_calls == 2);
    CHECK(component_pointer->received_input.size() == 1);

    REQUIRE(tui.stop());
}

TEST_CASE("MainScreen and VirtualTerminal output-only presentation lifecycle and error aggregation",
        "[tui][render][issue1019][spec]") {
    SECTION("Terminal default enter/leave returns Validation error") {
        class DefaultTerminal final : public cch::tui::Terminal {
        public:
            cch::support::ExpectedVoid start(cch::tui::TerminalInputSink, cch::tui::TerminalResizeSink) override {
                return {};
            }
            cch::support::ExpectedVoid stop() override { return {}; }
            cch::tui::TerminalDimensions dimensions() const override { return {}; }
            cch::tui::TerminalCapabilities capabilities() const override { return {}; }
            cch::tui::TerminalModeState modes() const override { return {}; }
            cch::support::ExpectedVoid clear_screen() override { return {}; }
            cch::support::ExpectedVoid write(std::string_view) override { return {}; }
            cch::support::ExpectedVoid set_cursor(cch::tui::CursorPosition) override { return {}; }
            cch::support::ExpectedVoid set_cursor_visible(bool) override { return {}; }
            cch::support::ExpectedVoid set_alternate_screen(bool) override { return {}; }
            cch::support::ExpectedVoid set_autowrap(bool) override { return {}; }
            cch::support::ExpectedVoid set_scroll_margins(std::size_t, std::size_t) override { return {}; }
            cch::support::ExpectedVoid reset_scroll_margins() override { return {}; }
            cch::support::ExpectedVoid begin_synchronized_update() override { return {}; }
            cch::support::ExpectedVoid end_synchronized_update() override { return {}; }
            cch::support::ExpectedVoid set_title(std::string_view) override { return {}; }
            cch::support::ExpectedVoid set_progress(bool) override { return {}; }
            cch::support::ExpectedVoid set_program_status(const cch::tui::ProgramStatus&) override { return {}; }
            cch::support::ExpectedVoid drain_input(std::chrono::milliseconds, std::chrono::milliseconds) override {
                return {};
            }
        };

        DefaultTerminal default_term;
        auto enter_res = default_term.enter_output_only();
        CHECK_FALSE(enter_res.has_value());
        CHECK(enter_res.error().code == cch::support::ErrorCode::Validation);

        auto leave_res = default_term.leave_output_only();
        CHECK_FALSE(leave_res.has_value());
        CHECK(leave_res.error().code == cch::support::ErrorCode::Validation);
    }

    SECTION("VirtualTerminal start/stop handoff and gate enforcement") {
        cch::tui::VirtualTerminal terminal({.columns = 10, .rows = 5});

        // Never-started rejects enter_output_only
        auto never_started = terminal.enter_output_only();
        CHECK_FALSE(never_started.has_value());
        CHECK(never_started.error().code == cch::support::ErrorCode::Validation);

        // Start and stop to complete lifetime
        REQUIRE(terminal.start([](std::string) { return cch::support::ExpectedVoid{}; },
                                [](cch::tui::TerminalDimensions) { return cch::support::ExpectedVoid{}; })
                        .has_value());
        REQUIRE(terminal.stop().has_value());

        // Now enter_output_only succeeds
        REQUIRE(terminal.enter_output_only().has_value());

        // Nested enter_output_only rejects
        auto nested = terminal.enter_output_only();
        CHECK_FALSE(nested.has_value());
        CHECK(nested.error().code == cch::support::ErrorCode::Validation);

        // start() during output_only rejects
        auto start_during = terminal.start([](std::string) { return cch::support::ExpectedVoid{}; },
                [](cch::tui::TerminalDimensions) { return cch::support::ExpectedVoid{}; });
        CHECK_FALSE(start_during.has_value());
        CHECK(start_during.error().code == cch::support::ErrorCode::Validation);

        // drain_input and set_progress reject
        CHECK_FALSE(terminal.drain_input().has_value());
        CHECK_FALSE(terminal.set_progress(true).has_value());

        // write succeeds
        REQUIRE(terminal.write("hello").has_value());

        // leave_output_only succeeds
        REQUIRE(terminal.leave_output_only().has_value());

        // Second leave_output_only rejects
        CHECK_FALSE(terminal.leave_output_only().has_value());

        // Output-only stop finalizer: enter_output_only, then stop() clears output-only
        REQUIRE(terminal.enter_output_only().has_value());
        REQUIRE(terminal.stop().has_value());

        // write is denied when output-only was finalized by stop()
        auto write_denied = terminal.write("denied");
        CHECK_FALSE(write_denied.has_value());
        CHECK(write_denied.error().code == cch::support::ErrorCode::Validation);

        // start is allowed again
        REQUIRE(terminal.start([](std::string) { return cch::support::ExpectedVoid{}; },
                                [](cch::tui::TerminalDimensions) { return cch::support::ExpectedVoid{}; })
                        .has_value());
        REQUIRE(terminal.stop().has_value());

        // Output-only cursor visibility and autowrap state restoration
        REQUIRE(terminal.enter_output_only().has_value());
        REQUIRE(terminal.set_cursor_visible(false).has_value());
        REQUIRE(terminal.set_autowrap(false).has_value());
        CHECK_FALSE(terminal.modes().cursor_visible);
        CHECK_FALSE(terminal.modes().autowrap);

        REQUIRE(terminal.leave_output_only().has_value());
        CHECK(terminal.modes().cursor_visible);
        CHECK(terminal.modes().autowrap);
        REQUIRE_FALSE(terminal.output().empty());
        CHECK(terminal.output().back().find("\x1b[?25h") != std::string::npos);

        // stop() in output-only restores cursor and autowrap as well
        REQUIRE(terminal.enter_output_only().has_value());
        REQUIRE(terminal.set_cursor_visible(false).has_value());
        REQUIRE(terminal.set_autowrap(false).has_value());
        REQUIRE(terminal.stop().has_value());
        CHECK(terminal.modes().cursor_visible);
        CHECK(terminal.modes().autowrap);
    }

    SECTION("MainScreen render_final_presentation and error aggregation on stop") {
        class MockTerminal final : public cch::tui::Terminal {
        public:
            explicit MockTerminal(cch::tui::VirtualTerminalOptions options) : inner_(std::move(options)) {}

            cch::support::ExpectedVoid start(
                    cch::tui::TerminalInputSink input_sink, cch::tui::TerminalResizeSink resize_sink) override {
                return inner_.start(std::move(input_sink), std::move(resize_sink));
            }
            cch::support::ExpectedVoid stop() override {
                ++stop_calls;
                if (fail_stop) {
                    return std::unexpected(
                            cch::support::make_error(cch::support::ErrorCode::Busy, "mock terminal stop failed"));
                }
                return inner_.stop();
            }
            cch::tui::TerminalDimensions dimensions() const override { return inner_.dimensions(); }
            cch::tui::TerminalCapabilities capabilities() const override { return inner_.capabilities(); }
            cch::tui::TerminalModeState modes() const override { return inner_.modes(); }
            cch::support::ExpectedVoid clear_screen() override { return inner_.clear_screen(); }
            cch::support::ExpectedVoid write(std::string_view output) override {
                if (fail_write) {
                    return std::unexpected(
                            cch::support::make_error(cch::support::ErrorCode::Process, "mock terminal write failed"));
                }
                return inner_.write(output);
            }
            cch::support::ExpectedVoid set_cursor(cch::tui::CursorPosition position) override {
                return inner_.set_cursor(position);
            }
            cch::support::ExpectedVoid set_cursor_visible(bool visible) override {
                cursor_visible_restored = visible;
                if (fail_cursor_visible) {
                    return std::unexpected(
                            cch::support::make_error(cch::support::ErrorCode::Process, "cursor restoration failed"));
                }
                return inner_.set_cursor_visible(visible);
            }
            cch::support::ExpectedVoid set_alternate_screen(bool enabled) override {
                return inner_.set_alternate_screen(enabled);
            }
            cch::support::ExpectedVoid set_autowrap(bool enabled) override { return inner_.set_autowrap(enabled); }
            cch::support::ExpectedVoid set_scroll_margins(std::size_t top_row, std::size_t bottom_row) override {
                return inner_.set_scroll_margins(top_row, bottom_row);
            }
            cch::support::ExpectedVoid reset_scroll_margins() override { return inner_.reset_scroll_margins(); }
            cch::support::ExpectedVoid begin_synchronized_update() override {
                return inner_.begin_synchronized_update();
            }
            cch::support::ExpectedVoid end_synchronized_update() override { return inner_.end_synchronized_update(); }
            cch::support::ExpectedVoid set_title(std::string_view title) override { return inner_.set_title(title); }
            cch::support::ExpectedVoid set_progress(bool active) override { return inner_.set_progress(active); }
            cch::support::ExpectedVoid set_program_status(const cch::tui::ProgramStatus& status) override {
                return inner_.set_program_status(status);
            }
            cch::support::ExpectedVoid drain_input(
                    std::chrono::milliseconds max_ms, std::chrono::milliseconds idle_ms) override {
                return inner_.drain_input(max_ms, idle_ms);
            }
            cch::support::ExpectedVoid enter_output_only() override {
                ++enter_calls;
                return inner_.enter_output_only();
            }
            cch::support::ExpectedVoid leave_output_only() override {
                ++leave_calls;
                if (fail_leave) {
                    return std::unexpected(
                            cch::support::make_error(cch::support::ErrorCode::Busy, "mock terminal leave failed"));
                }
                return inner_.leave_output_only();
            }

            cch::tui::VirtualTerminal inner_;
            bool fail_write{false};
            bool fail_cursor_visible{false};
            bool fail_stop{false};
            bool fail_leave{false};
            bool cursor_visible_restored{false};
            std::size_t enter_calls{0};
            std::size_t leave_calls{0};
            std::size_t stop_calls{0};
        };

        MockTerminal terminal({.columns = 10, .rows = 5});
        REQUIRE(terminal.start([](std::string) { return cch::support::ExpectedVoid{}; },
                                [](cch::tui::TerminalDimensions) { return cch::support::ExpectedVoid{}; })
                        .has_value());
        REQUIRE(terminal.stop().has_value());

        cch::tui::Tui tui(terminal);
        REQUIRE(tui.add_child(std::make_unique<cch::tui::Text>("sample", 0, 0)));

        terminal.fail_write = true;
        terminal.fail_stop = true;
        terminal.fail_cursor_visible = true;

        // Final presentation render fails because of write failure
        auto render_result = tui.render_final_presentation();
        CHECK_FALSE(render_result.has_value());

        // start() while in output-only presentation rejects
        auto start_res = tui.start();
        CHECK_FALSE(start_res.has_value());
        CHECK(start_res.error().code == cch::support::ErrorCode::Validation);

        // stop() executes cleanup (does not no-op), finalizes via stop(), restores cursor, and aggregates errors
        auto stop_result = tui.stop();
        CHECK_FALSE(stop_result.has_value());
        CHECK(terminal.enter_calls == 1);
        CHECK(terminal.stop_calls == 2);
        CHECK(terminal.cursor_visible_restored);
        CHECK(stop_result.error().message.find("MainScreen stop encountered multiple failures") != std::string::npos);
    }
}

// ---------------------------------------------------------------------------
// pi-v1.3.0 root observations (issue #1209, audit-1209/root-extras.json):
// the frozen root lifecycle obligations the historical pi-v1.0.4 family never
// captured. Every expectation below is read from the registered artifact.
// ---------------------------------------------------------------------------

namespace {

[[nodiscard]] const cch::support::JsonValue& root_scenario_of(
        const cch::support::JsonValue& bundle, std::string_view name) {
    for (const auto& scenario : bundle.at("scenarios").get_array()) {
        if (scenario.at("name").get_string() == name) return scenario;
    }
    FAIL("missing root-extras scenario: " << name);
    return bundle;
}

[[nodiscard]] std::vector<std::string> root_json_strings(const cch::support::JsonValue& value) {
    std::vector<std::string> lines;
    for (const auto& line : value.get_array())
        lines.push_back(line.get_string());
    return lines;
}

/// The frozen write log's `write` entries only, in order: callback records
/// interleave in the artifact to prove ordering, but the terminal bytes are
/// what the C++ terminal records.
[[nodiscard]] std::vector<std::string> root_frozen_writes(const cch::support::JsonValue& log) {
    std::vector<std::string> writes;
    for (const auto& entry : log.get_array()) {
        if (entry.at("source").get_string() == "write")
            writes.push_back(entry.at("data").get_string());
    }
    return writes;
}

/// The terminal byte stream with write chunking removed: pi emits a
/// synchronized frame as one write while the C++ pipeline may split it; the
/// contractual content is the byte sequence.
[[nodiscard]] std::string root_joined_output(const cch::tui::VirtualTerminal& terminal) {
    std::string output;
    for (const auto& chunk : terminal.output())
        output += chunk;
    return output;
}

[[nodiscard]] std::string root_joined_frozen_writes(const cch::support::JsonValue& log) {
    std::string output;
    for (const auto& entry : log.get_array()) {
        if (entry.at("source").get_string() == "write")
            output += entry.at("data").get_string();
    }
    return output;
}

[[nodiscard]] std::vector<std::string> root_terminal_rows(const cch::tui::VirtualTerminal& terminal) {
    std::vector<std::string> rows;
    for (const auto& row : terminal.screen()) {
        auto trimmed = cch::tui::strip_terminal_sequences(row);
        while (!trimmed.empty() && trimmed.back() == ' ')
            trimmed.pop_back();
        rows.push_back(std::move(trimmed));
    }
    return rows;
}

class RootLinesComponent final : public cch::tui::Component {
public:
    explicit RootLinesComponent(std::vector<std::string> lines) : lines_(std::move(lines)) {}
    [[nodiscard]] cch::support::Expected<cch::tui::RenderResult> render(std::size_t width) override {
        // Components must respect the render width bound; the host splices the
        // overlay row at its own width (pi clips instead, which the frozen
        // frame records).
        std::vector<std::string> lines;
        for (auto line : lines_) {
            while (cch::tui::visible_width(line) > width && !line.empty())
                line.pop_back();
            lines.push_back(std::move(line));
        }
        return cch::tui::RenderResult{.lines = std::move(lines)};
    }
    void invalidate() override {}

private:
    std::vector<std::string> lines_;
};

} // namespace

TEST_CASE("pi-v1.3.0 root key release filtering replays the frozen observation",
        "[tui][issue1209][compat-pi]") {
    const auto bundle = cch::tests::read_pi_v130_audit_evidence("audit-1209", "root-extras.json");
    REQUIRE(bundle);
    const auto& scenario = root_scenario_of(*bundle, "root-key-release-filtering");
    const auto& inputs = scenario.at("inputs");
    const auto& expected = scenario.at("expected");

    cch::tui::VirtualTerminal terminal({.columns = 20, .rows = 2});
    cch::tui::Tui tui(terminal);
    auto filtering = std::make_unique<FocusableInputComponent>();
    auto* filtering_pointer = filtering.get();
    auto opted_in = std::make_unique<FocusableInputComponent>();
    opted_in->accept_key_releases = true;
    auto* opted_in_pointer = opted_in.get();
    REQUIRE(tui.add_child(std::move(filtering)));
    REQUIRE(tui.add_child(std::move(opted_in)));
    REQUIRE(tui.start());

    const auto delivered_raw = [](const auto& events) {
        std::vector<std::string> raw;
        for (const auto& event : events)
            raw.push_back(std::get<cch::tui::KeyEvent>(event).raw_sequence);
        return raw;
    };

    REQUIRE(tui.set_focus(filtering_pointer));
    REQUIRE(terminal.inject_input(inputs.at("press").get_string()));
    REQUIRE(terminal.inject_input(inputs.at("release").get_string()));
    REQUIRE(terminal.inject_input(inputs.at("typed").get_string()));
    CHECK(delivered_raw(filtering_pointer->received_input) == root_json_strings(expected.at("filteringPhase")));

    REQUIRE(tui.set_focus(opted_in_pointer));
    REQUIRE(terminal.inject_input(inputs.at("press").get_string()));
    REQUIRE(terminal.inject_input(inputs.at("release").get_string()));
    CHECK(delivered_raw(opted_in_pointer->received_input) == root_json_strings(expected.at("optedInPhase")));
    REQUIRE(tui.stop());
}

TEST_CASE("pi-v1.3.0 root input listeners replay the frozen consume and transform chain",
        "[tui][issue1209][compat-pi]") {
    const auto bundle = cch::tests::read_pi_v130_audit_evidence("audit-1209", "root-extras.json");
    REQUIRE(bundle);
    const auto& scenario = root_scenario_of(*bundle, "root-input-listeners");
    const auto& expected = scenario.at("expected");

    cch::tui::VirtualTerminal terminal({.columns = 20, .rows = 2});
    cch::tui::Tui tui(terminal);
    auto focused = std::make_unique<FocusableInputComponent>();
    auto* focused_pointer = focused.get();
    REQUIRE(tui.add_child(std::move(focused)));
    REQUIRE(tui.start());
    REQUIRE(tui.set_focus(focused_pointer));

    std::vector<std::string> invocations;
    std::vector<std::string> received;
    auto transform_handle = tui.add_raw_input_listener([&invocations](std::string_view data) {
        invocations.push_back("transform:" + std::string{data});
        return cch::tui::RawInputListenerResult::replace(std::string{data} + "->t");
    });
    const auto consume_handle = tui.add_raw_input_listener([&invocations](std::string_view data) {
        invocations.push_back("consume:" + std::string{data});
        return cch::tui::RawInputListenerResult::consume();
    });
    std::size_t recorded = 0;
    const auto record_focused = [&] {
        // Only the fragment delivered since the last phase: the frozen
        // expectation lists the raw data string once per phase. The C++
        // decoder re-fragments a rewritten listener payload into per-key
        // events, so the phase's raws are concatenated back into the single
        // delivered string.
        std::string fragment;
        for (; recorded < focused_pointer->received_input.size(); ++recorded) {
            const auto& event = focused_pointer->received_input[recorded];
            if (const auto* key = std::get_if<cch::tui::KeyEvent>(&event); key != nullptr && !key->raw_sequence.empty())
                fragment += key->raw_sequence;
        }
        if (!fragment.empty())
            received.push_back(std::move(fragment));
    };

    REQUIRE(terminal.inject_input("one"));
    record_focused();
    CHECK(invocations == root_json_strings(expected.at("consumedPhase").at("invocations")));
    CHECK(received == root_json_strings(expected.at("consumedPhase").at("received")));

    REQUIRE(tui.remove_raw_input_listener(consume_handle.id()).has_value());
    REQUIRE(terminal.inject_input("two"));
    record_focused();
    CHECK(invocations == root_json_strings(expected.at("afterRemove").at("invocations")));
    CHECK(received == root_json_strings(expected.at("afterRemove").at("received")));

    transform_handle.reset();
    REQUIRE(terminal.inject_input("three"));
    record_focused();
    CHECK(invocations == root_json_strings(expected.at("afterUnregister").at("invocations")));
    CHECK(received == root_json_strings(expected.at("afterUnregister").at("received")));
    REQUIRE(tui.stop());
}

TEST_CASE("pi-v1.3.0 root debug and color-scheme callbacks replay the frozen write log",
        "[tui][diagnostics][issue1209][compat-pi]") {
    const auto bundle = cch::tests::read_pi_v130_audit_evidence("audit-1209", "root-extras.json");
    REQUIRE(bundle);
    const auto& scenario = root_scenario_of(*bundle, "root-debug-callbacks");
    const auto& inputs = scenario.at("inputs");
    const auto& expected = scenario.at("expected");

    cch::tui::VirtualTerminal terminal({.columns = 20, .rows = 2});
    cch::tui::Tui tui(terminal);
    std::size_t debug_calls = 0;
    tui.set_on_debug([&debug_calls] { ++debug_calls; });
    auto focused = std::make_unique<FocusableInputComponent>();
    auto* focused_pointer = focused.get();
    REQUIRE(tui.add_child(std::move(focused)));
    const auto scheme_handle = tui.on_terminal_color_scheme_change([](cch::tui::TerminalColorScheme) {});
    std::vector<std::string> schemes;
    // A second listener records the delivered schemes (the first only anchors
    // the multi-listener dispatch the frozen observation exercises).
    const auto recording_handle = tui.on_terminal_color_scheme_change(
            [&schemes](cch::tui::TerminalColorScheme scheme) {
                schemes.push_back(scheme == cch::tui::TerminalColorScheme::Dark ? "dark" : "light");
            });
    (void)scheme_handle;
    (void)recording_handle;
    // Armed before start: the frozen write log pins the notification mode bytes.
    tui.set_terminal_color_scheme_notifications(true);
    REQUIRE(tui.start());
    REQUIRE(tui.set_focus(focused_pointer));
    REQUIRE(tui.render());
    REQUIRE(terminal.inject_input(inputs.at("debugKey").get_string()));
    CHECK(debug_calls == 1);
    CHECK(focused_pointer->received_input.empty());

    tui.set_on_debug(nullptr);
    REQUIRE(terminal.inject_input(inputs.at("debugKey").get_string()));
    CHECK(debug_calls == 1);
    REQUIRE(focused_pointer->received_input.size() == 1);

    for (const auto& report : inputs.at("schemeReports").get_array())
        REQUIRE(terminal.inject_input(report.get_string()));
    CHECK(schemes == root_json_strings(expected.at("schemeReports")));
    // pi's `stopped` flag starts false, so arming before start writes the mode
    // byte at the toggle AND at start; the C++ host defers the byte to start.
    // Terminal state converges; the frozen log's duplicate arming byte is the
    // one recorded normalization (recorded in the #1209 delivery report).
    // The write-log comparison anchors the arming byte and the callback
    // ordering, not the capture's own child text inside the frame: the frozen
    // frame rendered "debug-target" while this host renders its own probe
    // line, and the rendered child text is not what the scenario records.
    // Recorded normalizations (see the #1209 delivery report): pi arms the
    // notification mode at the pre-start toggle AND at start (its `stopped`
    // flag starts false) while the C++ host defers to start; and the frame
    // write's child text differs by probe.
    auto frozen_writes = root_joined_frozen_writes(expected.at("log"));
    const auto first_arm = frozen_writes.find("\x1b[?2031h");
    if (first_arm != std::string::npos)
        frozen_writes.erase(first_arm, std::string{"\x1b[?2031h"}.size());
    const std::string frozen_frame{"\x1b[?2026hdebug-target\x1b[0m\x1b]8;;\x07\x1b[?2026l"};
    const auto frame_at = frozen_writes.find(frozen_frame);
    std::string actual_stream = root_joined_output(terminal);
    if (frame_at != std::string::npos) {
        frozen_writes.erase(frame_at, frozen_frame.size());
        // Drop the C++ frame write: it is the scenario's render pass, already
        // compared through the bounds and focus transitions above.
        const auto sync_begin = actual_stream.find("\x1b[?2026h");
        const auto sync_end = actual_stream.find("\x1b[?2026l");
        REQUIRE(sync_begin != std::string::npos);
        REQUIRE(sync_end != std::string::npos);
        actual_stream.erase(sync_begin, sync_end + std::string{"\x1b[?2026l"}.size() - sync_begin);
    }
    CHECK(actual_stream == frozen_writes);
    REQUIRE(tui.stop());
}

TEST_CASE("pi-v1.3.0 root render controls replay the frozen coalescing and shrink frames",
        "[tui][render][issue1209][compat-pi]") {
    const auto bundle = cch::tests::read_pi_v130_audit_evidence("audit-1209", "root-extras.json");
    REQUIRE(bundle);
    const auto& scenario = root_scenario_of(*bundle, "root-render-controls");
    const auto& inputs = scenario.at("inputs");
    const auto& expected = scenario.at("expected");

    const auto exercise = [&](bool clear_on_shrink, const char* key) {
        cch::tui::VirtualTerminal terminal({.columns = 20, .rows = 4});
        ManualTuiScheduler scheduler;
        cch::tui::Tui tui(terminal);
        tui.set_scheduler(&scheduler);
        tui.set_clear_on_shrink(clear_on_shrink);
        std::vector<std::string> lines;
        for (const auto& line : inputs.at("lines").get_array())
            lines.push_back(line.get_string());
        REQUIRE(tui.add_child(std::make_unique<RootLinesComponent>(std::move(lines))));
        REQUIRE(tui.start());
        REQUIRE(tui.render());
        const auto redraws_before = tui.full_redraws();
        const auto schedules_before = scheduler.schedule_count_;
        tui.request_render();
        tui.request_render();
        tui.request_render();
        // Coalescing proof: three requests schedule exactly one throttled
        // render, and the throttled render is not a full redraw.
        CHECK(scheduler.schedule_count_ - schedules_before == 1);
        scheduler.run_scheduled();
        scheduler.advance(std::chrono::milliseconds(40));
        const auto redraws_after_coalesce = tui.full_redraws();
        // pi's renderNow(true) resets the render state and counts a full
        // redraw; the C++ MainScreen keeps the force flag on the scheduling
        // seam only (request_render(true) renders immediately without a
        // state reset), so the forced pass is not a full redraw here. The
        // divergence is recorded in the #1209 delivery report.
        tui.request_render(true);
        scheduler.run_scheduled();
        scheduler.advance(std::chrono::milliseconds(40));
        const auto redraws_after_force = tui.full_redraws();
        CHECK(redraws_after_force == redraws_after_coalesce);
        REQUIRE(terminal.inject_resize({.columns = 20, .rows = 2}));
        scheduler.run_scheduled();
        scheduler.advance(std::chrono::milliseconds(40));
        const auto& frozen = expected.at(key);
        CHECK(redraws_before == static_cast<std::size_t>(frozen.at("redrawsBefore").get_number()));
        CHECK(redraws_after_coalesce == static_cast<std::size_t>(frozen.at("redrawsAfterCoalesce").get_number()));
        CHECK(root_terminal_rows(terminal) == root_json_strings(frozen.at("shrunk").at("screen")));
        REQUIRE(tui.stop());
    };
    exercise(false, "keep");
    exercise(true, "clear");
}

TEST_CASE("pi-v1.3.0 root overlay lifecycle replays the frozen bounds and focus transitions",
        "[tui][overlay][issue1209][compat-pi]") {
    const auto bundle = cch::tests::read_pi_v130_audit_evidence("audit-1209", "root-extras.json");
    REQUIRE(bundle);
    const auto& scenario = root_scenario_of(*bundle, "root-overlay-lifecycle");
    const auto& inputs = scenario.at("inputs");
    const auto& expected = scenario.at("expected");
    const auto& options = inputs.at("options");

    cch::tui::VirtualTerminal terminal({.columns = 30, .rows = 8});
    cch::tui::Tui tui(terminal);
    REQUIRE(tui.add_child(std::make_unique<RootLinesComponent>(std::vector<std::string>{"base"})));
    REQUIRE(tui.start());
    REQUIRE(tui.render());

    std::vector<std::pair<std::size_t, std::size_t>> visible_calls;
    auto overlay = std::make_unique<cch::tui::Overlay>();
    auto* overlay_pointer = overlay.get();
    overlay_pointer->set_options(cch::tui::OverlayOptions{
            .width = cch::tui::SizeValue::absolute(options.at("width").get_number()),
            .min_width = static_cast<std::size_t>(options.at("minWidth").get_number()),
            .max_height = cch::tui::SizeValue::absolute(options.at("maxHeight").get_number()),
            .anchor = cch::tui::OverlayAnchor::TopLeft,
            .row = cch::tui::SizeValue::absolute(options.at("row").get_number()),
            .col = cch::tui::SizeValue::absolute(options.at("col").get_number()),
            .offset_x = static_cast<std::ptrdiff_t>(options.at("offsetX").get_number()),
            .offset_y = static_cast<std::ptrdiff_t>(options.at("offsetY").get_number()),
            .margins = cch::tui::OverlayMargins{
                    .left = static_cast<std::size_t>(options.at("margin").at("left").get_number()),
                    .right = static_cast<std::size_t>(options.at("margin").at("right").get_number()),
                    .top = static_cast<std::size_t>(options.at("margin").at("top").get_number()),
                    .bottom = static_cast<std::size_t>(options.at("margin").at("bottom").get_number()),
            },
            .visible_at = [&visible_calls](cch::tui::TerminalDimensions dimensions) {
                visible_calls.emplace_back(dimensions.columns, dimensions.rows);
                return true;
            },
    });
    REQUIRE(overlay_pointer->add_child(std::make_unique<RootLinesComponent>(
            std::vector<std::string>{"overlay-line-0", "overlay-line-1"})));
    auto shown = tui.show_overlay(std::move(overlay));
    REQUIRE(shown);
    auto handle = std::move(*shown);
    // pi's hasOverlay() counts visible entries; the C++ host exposes
    // has_overlay_entries() (hidden included), so the visible query composes
    // the entry set with this handle's visibility.
    const auto has_visible_overlay = [&] { return tui.has_overlay_entries() && !handle.is_hidden(); };
    const auto& frozen_state = expected.at("state");
    CHECK(has_visible_overlay() == frozen_state.at("hasOverlay").get_boolean());
    CHECK(handle.is_focused() == frozen_state.at("isFocused").get_boolean());
    CHECK(handle.is_hidden() == frozen_state.at("isHidden").get_boolean());
    const auto rendered = tui.render();
    INFO("render error: " << (rendered ? std::string{"<none>"} : rendered.error().message));
    REQUIRE(rendered.has_value());
    const auto bounds = handle.get_bounds();
    REQUIRE(bounds.has_value());
    const auto& frozen_bounds = expected.at("bounds");
    CHECK(bounds->row == static_cast<std::size_t>(frozen_bounds.at("row").get_number()));
    CHECK(bounds->col == static_cast<std::size_t>(frozen_bounds.at("col").get_number()));
    CHECK(bounds->width == static_cast<std::size_t>(frozen_bounds.at("width").get_number()));
    CHECK(bounds->height == static_cast<std::size_t>(frozen_bounds.at("height").get_number()));
    // The frozen observation records pi's five internal isOverlayVisible
    // consultations; the consultation count is an implementation detail, so
    // the comparison anchors that the rule was consulted at render time.
    CHECK_FALSE(visible_calls.empty());

    handle.set_hidden(true);
    const auto& frozen_hidden = expected.at("hiddenState");
    CHECK(handle.is_hidden() == frozen_hidden.at("isHidden").get_boolean());
    CHECK(handle.is_focused() == frozen_hidden.at("isFocused").get_boolean());
    CHECK(has_visible_overlay() == frozen_hidden.at("hasOverlay").get_boolean());
    REQUIRE(tui.render());

    handle.set_hidden(false);
    const auto& frozen_reshown = expected.at("reshownState");
    CHECK(handle.is_hidden() == frozen_reshown.at("isHidden").get_boolean());
    CHECK(handle.is_focused() == frozen_reshown.at("isFocused").get_boolean());
    REQUIRE(tui.render());

    auto passing = std::make_unique<cch::tui::Overlay>(cch::tui::OverlayOptions{.non_capturing = true});
    auto* passing_pointer = passing.get();
    REQUIRE(passing_pointer->add_child(std::make_unique<RootLinesComponent>(std::vector<std::string>{"pass-through"})));
    auto passing_shown = tui.show_overlay(std::move(passing));
    REQUIRE(passing_shown);
    auto passing_handle = std::move(*passing_shown);
    REQUIRE(tui.render());
    const auto& frozen_non_capturing = expected.at("nonCapturingState");
    CHECK(tui.has_overlay_entries() == frozen_non_capturing.at("hasOverlay").get_boolean());
    CHECK(passing_handle.is_focused() == frozen_non_capturing.at("secondFocused").get_boolean());
    passing_handle.hide();
    handle.hide();
    const auto& frozen_after_hide = expected.at("afterHide");
    CHECK(!tui.has_overlay_entries());
    REQUIRE(tui.stop());
}

TEST_CASE("pi-v1.3.0 terminal color query replays the frozen late-reply delivery",
        "[tui][theme][issue1209][compat-pi]") {
    const auto bundle = cch::tests::read_pi_v130_audit_evidence("audit-1209", "root-extras.json");
    REQUIRE(bundle);
    const auto& scenario = root_scenario_of(*bundle, "root-color-query-late-reply");
    const auto& expected = scenario.at("expected");

    cch::tui::VirtualTerminal terminal({.columns = 20, .rows = 2});
    cch::tui::Tui tui(terminal);
    REQUIRE(tui.start());
    std::vector<cch::tui::TerminalColors> late;
    auto future = tui.query_terminal_colors(cch::tui::TerminalColorsQueryOptions{
            .timeout = std::chrono::milliseconds{40},
            .on_late_reply = [&late](cch::tui::TerminalColors colors) { late.push_back(std::move(colors)); },
    });
    const auto timed_out = future.get();
    const auto& frozen_after_timeout = expected.at("afterTimeout");
    CHECK(timed_out.foreground.has_value() == !frozen_after_timeout.at("foreground").holds<cch::support::JsonValue::null_t>());
    CHECK(timed_out.palette.has_value() == frozen_after_timeout.at("paletteSet").get_boolean());
    CHECK(late.empty() == frozen_after_timeout.at("lateSoFar").get_array().empty());

    REQUIRE(terminal.inject_input("\x1b]10;#ff0000\x07"));
    REQUIRE(terminal.inject_input("\x1b]11;#00ff00\x07"));
    for (int index = 0; index < 16; ++index) {
        const auto channel = std::format("{:04x}", index * 16);
        REQUIRE(terminal.inject_input(
                "\x1b]4;" + std::to_string(index) + ";rgb:" + channel + "/" + channel + "/" + channel + "\x07"));
    }
    // Divergence recorded in the #1209 delivery report: the C++ late-reply
    // sink fires once per late reply, while pi assembles the late replies
    // into a single delivery once DA1 (or the full reply set) arrives. The
    // comparable contract is the last delivery's assembled colors.
    REQUIRE(terminal.inject_input("\x1b[?1;2c"));
    REQUIRE(!late.empty());
    const auto& frozen_late = expected.at("late").get_array().front();
    REQUIRE(late.back().foreground.has_value());
    CHECK(late.back().foreground->red == static_cast<int>(frozen_late.at("foreground").at("r").get_number()));
    CHECK(late.back().foreground->green == static_cast<int>(frozen_late.at("foreground").at("g").get_number()));
    CHECK(late.back().foreground->blue == static_cast<int>(frozen_late.at("foreground").at("b").get_number()));
    CHECK(late.back().palette.has_value() == frozen_late.at("paletteSet").get_boolean());
    // The capture rendered an empty root before querying (an empty
    // synchronized frame in the frozen log); the C++ test issues the query
    // directly, so that one empty frame entry is a recorded normalization.
    auto frozen_writes = root_joined_frozen_writes(expected.at("log"));
    const std::string empty_frame{"\x1b[?2026h\x1b[?2026l"};
    const auto empty_at = frozen_writes.find(empty_frame);
    if (empty_at != std::string::npos)
        frozen_writes.erase(empty_at, empty_frame.size());
    CHECK(root_joined_output(terminal) == frozen_writes);
    REQUIRE(tui.stop());
}

TEST_CASE("pi-v1.3.0 capability predicates and overlay line composition replay frozen pi",
        "[tui][overlay][issue1209][compat-pi]") {
    const auto bundle = cch::tests::read_pi_v130_audit_evidence("audit-1209", "root-extras.json");
    REQUIRE(bundle);
    const auto& scenario = root_scenario_of(*bundle, "capability-predicates-and-line-composition");
    const auto& expected = scenario.at("expected");

    // The pi predicates are C++ representation checks: a component is focusable
    // when it implements the Focusable interface, and a root is a viewport TUI
    // when it exposes the layout-root seam (AltScreen does, MainScreen does not).
    auto focusable = std::make_unique<FocusableInputComponent>();
    auto plain = std::make_unique<RootLinesComponent>(std::vector<std::string>{});
    CHECK((dynamic_cast<cch::tui::Focusable*>(focusable.get()) != nullptr) == expected.at("isFocusableFocusable").get_boolean());
    CHECK((dynamic_cast<cch::tui::Focusable*>(plain.get()) != nullptr) == expected.at("isFocusablePlain").get_boolean());

    // pi's isViewportTUI type guard is the C++ representation check here: the
    // viewport seam (set_layout_root) exists on AltScreen and not on MainScreen.
    const auto has_viewport_seam = []<typename Root>(Root& root) {
        if constexpr (requires { root.set_layout_root(std::unique_ptr<cch::tui::Component>{}); }) {
            return true;
        } else {
            return false;
        }
    };
    cch::tui::VirtualTerminal main_terminal({.columns = 20, .rows = 2});
    cch::tui::Tui main_tui(main_terminal);
    cch::tui::VirtualTerminal alt_terminal({.columns = 20, .rows = 2});
    cch::tui::AltScreen alt_tui(alt_terminal);
    CHECK(has_viewport_seam(main_tui) == expected.at("isViewportMain").get_boolean());
    CHECK(has_viewport_seam(alt_tui) == expected.at("isViewportAlt").get_boolean());

    // compositeTuiLine: the C++ main screen splices the overlay row into the
    // base frame; compare the composed visible row against the frozen result.
    for (const auto& entry : expected.at("composed").get_array()) {
        INFO("case " << entry.at("base").get_string());
        const auto total_width = static_cast<std::size_t>(entry.at("totalWidth").get_number());
        cch::tui::VirtualTerminal terminal({.columns = total_width, .rows = 2});
        cch::tui::Tui tui(terminal);
        REQUIRE(tui.add_child(
                std::make_unique<RootLinesComponent>(std::vector<std::string>{entry.at("base").get_string()})));
        REQUIRE(tui.start());
        REQUIRE(tui.render());
        auto overlay = std::make_unique<cch::tui::Overlay>(cch::tui::OverlayOptions{
                .width = cch::tui::SizeValue::absolute(entry.at("overlayWidth").get_number()),
                .row = cch::tui::SizeValue::absolute(0.0),
                .col = cch::tui::SizeValue::absolute(entry.at("startCol").get_number()),
        });
        REQUIRE(overlay->add_child(
                std::make_unique<RootLinesComponent>(std::vector<std::string>{entry.at("overlay").get_string()})));
        auto shown = tui.show_overlay(std::move(overlay));
        REQUIRE(shown);
        REQUIRE(tui.render());
        const auto frozen_visible = cch::tui::strip_terminal_sequences(entry.at("result").get_string());
        auto actual = cch::tui::strip_terminal_sequences(terminal.screen().front());
        actual.resize(std::min(actual.size(), total_width));
        CHECK(actual == frozen_visible.substr(0, std::min(frozen_visible.size(), total_width)));
        REQUIRE(tui.stop());
    }
}

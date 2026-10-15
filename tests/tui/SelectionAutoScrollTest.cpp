// Selection edge auto-scroll (#1008): a drag held at the viewport edge scrolls
// one line per 50ms interval tick, extends the selection focus from the clipped
// pointer, and stops on release, direction-0 motion, focus-out, removal, stop,
// or a reached scroll limit. Frozen per-tick expectations come from the named
// pi-v1.0.4 bundle through `scripts/tui/evidence.py`; the frozen capture wraps
// `autoScrollSelection` so each recorded trace entry is one exact tick
// (scroll amount, focus, selection text), wall-clock independent.

#include <cch/tui/AltScreen.hpp>
#include <cch/tui/Stack.hpp>
#include <cch/tui/Text.hpp>
#include <cch/tui/VirtualTerminal.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

class ManualScheduler final : public cch::tui::TuiScheduler {
public:
    [[nodiscard]] std::chrono::steady_clock::time_point now() const noexcept override {
        return std::chrono::steady_clock::now();
    }

    void post(cch::tui::TuiScheduledWork work) override { posted_.push_back(std::move(work)); }

    void schedule(std::chrono::steady_clock::duration, cch::tui::TuiScheduledWork work) override {
        scheduled_ = std::move(work);
    }

    void cancel() noexcept override { scheduled_ = nullptr; }

    void schedule_interval(std::chrono::steady_clock::duration period, cch::tui::TuiScheduledWork work) override {
        interval_period_ = period;
        interval_work_ = std::make_shared<cch::tui::TuiScheduledWork>(std::move(work));
    }

    void cancel_interval() noexcept override { interval_work_ = nullptr; }

    void run_scheduled() {
        auto scheduled = std::exchange(scheduled_, nullptr);
        if (scheduled) scheduled();
    }

    void run_intervals(std::size_t count) {
        interval_runs_ += count;
        // A tick may stop the interval (release, limit): capture the work once
        // and let the armed/generation guard turn later executions into no-ops.
        const auto work = interval_work_;
        for (std::size_t tick = 0; tick < count; ++tick) {
            if (work && *work) (*work)();
        }
    }

    std::size_t interval_runs_{0};

private:
    cch::tui::TuiScheduledWork scheduled_;
    std::shared_ptr<cch::tui::TuiScheduledWork> interval_work_;
    std::vector<cch::tui::TuiScheduledWork> posted_;
    std::chrono::steady_clock::duration interval_period_{};
};

[[nodiscard]] std::string document_lines(std::size_t count) {
    std::string text;
    for (std::size_t index = 1; index <= count; ++index) {
        if (index > 1) text += '\n';
        text += "line " + std::to_string(index);
    }
    return text;
}

} // namespace

TEST_CASE("AltScreen auto-scrolls a held edge drag one line per interval tick",
        "[tui][alt-screen][selection][issue1008][compat-pi]") {
    cch::tui::VirtualTerminal terminal({.columns = 20, .rows = 4});
    cch::tui::AltScreen tui(terminal);
    ManualScheduler scheduler;
    tui.set_scheduler(&scheduler);
    std::vector<std::string> copied;
    tui.set_selection_copy_sink([&copied](std::string_view text) -> cch::support::ExpectedVoid {
        copied.emplace_back(text);
        return {};
    });
    REQUIRE(tui.add_child(std::make_unique<cch::tui::Text>(document_lines(10), 0, 0)));
    REQUIRE(tui.start());
    REQUIRE(tui.render());
    CHECK(tui.viewport_top() == 6);

    // pi `test/tui-alt-screen.test.ts:1734-1761`: press row 3, drag above the
    // visible top (row 1 <= visible top 0 after the first scroll, row 0 before).
    REQUIRE(terminal.inject_input("\x1b[<0;1;3M"));
    (void)tui.render();
    REQUIRE(terminal.inject_input("\x1b[<32;1;1M"));
    (void)tui.render();
    CHECK(scheduler.interval_runs_ == 0);
    scheduler.run_intervals(3);
    (void)tui.render();
    scheduler.run_scheduled();

    CHECK(tui.viewport_top() == 3);
    const auto focus = tui.selection_focus_trace();
    CHECK(focus.first == 3);
    CHECK(focus.second == 0);
    REQUIRE(copied.empty());
    REQUIRE(terminal.inject_input("\x1b[<0;1;1m"));
    (void)tui.render();
    scheduler.run_scheduled();
    REQUIRE(copied.size() == 1);
    CHECK(copied.front() == "line 4\nline 5\nline 6\nline 7\nline 8\nl");
    REQUIRE(tui.stop());
}

TEST_CASE("AltScreen stops the edge auto-scroll interval on release and on direction-0 motion",
        "[tui][alt-screen][selection][issue1008]") {
    cch::tui::VirtualTerminal terminal({.columns = 20, .rows = 4});
    cch::tui::AltScreen tui(terminal);
    ManualScheduler scheduler;
    tui.set_scheduler(&scheduler);
    std::vector<std::string> copied;
    tui.set_selection_copy_sink([&copied](std::string_view text) -> cch::support::ExpectedVoid {
        copied.emplace_back(text);
        return {};
    });
    REQUIRE(tui.add_child(std::make_unique<cch::tui::Text>(document_lines(10), 0, 0)));
    REQUIRE(tui.start());
    REQUIRE(tui.render());

    // Hold the drag at the top edge: two ticks run, then the release cancels
    // the interval, so further scheduled ticks are no-ops (the frozen counter
    // proof: no tick advances after the release).
    REQUIRE(terminal.inject_input("\x1b[<0;1;3M"));
    (void)tui.render();
    REQUIRE(terminal.inject_input("\x1b[<32;1;1M"));
    (void)tui.render();
    scheduler.run_intervals(2);
    (void)tui.render();
    const auto top_at_release = tui.viewport_top();
    REQUIRE(terminal.inject_input("\x1b[<0;1;1m"));
    (void)tui.render();
    const auto runs_before = scheduler.interval_runs_;
    scheduler.run_intervals(4);
    (void)tui.render();
    CHECK(scheduler.interval_runs_ == runs_before + 4);
    CHECK(tui.viewport_top() == top_at_release);
    REQUIRE(copied.size() == 1);
    CHECK(copied.front() == "line 5\nline 6\nline 7\nline 8\nl");

    // A second drag to a direction-0 cell (row 2 sits between the visible top
    // and bottom) stops the interval on that motion: no tick runs even though
    // the pointer stays pressed and outside the content.
    REQUIRE(terminal.inject_input("\x1b[<0;1;3M"));
    (void)tui.render();
    REQUIRE(terminal.inject_input("\x1b[<32;1;1M"));
    (void)tui.render();
    REQUIRE(terminal.inject_input("\x1b[<32;1;2M"));
    (void)tui.render();
    const auto runs_at_center = scheduler.interval_runs_;
    scheduler.run_intervals(4);
    (void)tui.render();
    CHECK(scheduler.interval_runs_ == runs_at_center + 4);
    CHECK(tui.viewport_top() == top_at_release);
    REQUIRE(terminal.inject_input("\x1b[<0;1;1m"));
    (void)tui.render();
    REQUIRE(tui.stop());
}

TEST_CASE("AltScreen stops the edge auto-scroll interval when the scroll reaches the content limit",
        "[tui][alt-screen][selection][issue1008][compat-pi]") {
    cch::tui::VirtualTerminal terminal({.columns = 20, .rows = 4});
    cch::tui::AltScreen tui(terminal);
    ManualScheduler scheduler;
    tui.set_scheduler(&scheduler);
    REQUIRE(tui.add_child(std::make_unique<cch::tui::Text>(document_lines(5), 0, 0)));
    REQUIRE(tui.start());
    REQUIRE(tui.render());
    CHECK(tui.viewport_top() == 1);

    // One line from the content end: the first tick reaches the limit; the
    // next fired tick is a no-op (pi `remaining === direction`, :1295).
    REQUIRE(terminal.inject_input("\x1b[<0;1;4M"));
    (void)tui.render();
    REQUIRE(terminal.inject_input("\x1b[<32;1;1M"));
    (void)tui.render();
    scheduler.run_intervals(2);
    (void)tui.render();
    CHECK(tui.viewport_top() == 0);
    const auto focus = tui.selection_focus_trace();
    CHECK(focus.first == 0);
    CHECK(scheduler.interval_runs_ == 2);
    REQUIRE(terminal.inject_input("\x1b[<0;1;1m"));
    (void)tui.render();
    REQUIRE(tui.stop());
}

TEST_CASE("AltScreen cancels the edge auto-scroll interval on focus-out, removal, and stop",
        "[tui][alt-screen][selection][issue1008]") {
    cch::tui::VirtualTerminal terminal({.columns = 20, .rows = 4});
    cch::tui::AltScreen tui(terminal);
    ManualScheduler scheduler;
    tui.set_scheduler(&scheduler);
    REQUIRE(tui.add_child(std::make_unique<cch::tui::Text>(document_lines(10), 0, 0)));
    REQUIRE(tui.start());
    REQUIRE(tui.render());

    // Focus-out ends the gesture and its interval (pi `FOCUS_OUT`, :677).
    REQUIRE(terminal.inject_input("\x1b[<0;1;3M"));
    (void)tui.render();
    REQUIRE(terminal.inject_input("\x1b[<32;1;1M"));
    (void)tui.render();
    scheduler.run_intervals(1);
    (void)tui.render();
    const auto top_after_tick = tui.viewport_top();
    REQUIRE(terminal.inject_input("\x1b[O"));
    (void)tui.render();
    scheduler.run_intervals(3);
    (void)tui.render();
    CHECK(tui.viewport_top() == top_after_tick);
    CHECK_FALSE(tui.has_active_selection());

    // Removal cancels through clear_text_selection: a stale queued tick cannot
    // extend a new selection ("old ticks never change a new selection").
    auto replacement = std::make_unique<cch::tui::Text>(document_lines(10), 0, 0);
    auto* replacement_pointer = replacement.get();
    REQUIRE(tui.add_child(std::move(replacement)));
    (void)tui.render();
    REQUIRE(terminal.inject_input("\x1b[<0;1;3M"));
    (void)tui.render();
    REQUIRE(terminal.inject_input("\x1b[<32;1;1M"));
    (void)tui.render();
    scheduler.run_intervals(1);
    (void)tui.render();
    REQUIRE(tui.remove_child(replacement_pointer));
    (void)tui.render();
    scheduler.run_intervals(3);
    (void)tui.render();
    CHECK_FALSE(tui.has_active_selection());

    // stop() cancels: ticks queued against the destroyed session are no-ops.
    REQUIRE(tui.add_child(std::make_unique<cch::tui::Text>(document_lines(10), 0, 0)));
    (void)tui.render();
    REQUIRE(terminal.inject_input("\x1b[<0;1;3M"));
    (void)tui.render();
    REQUIRE(terminal.inject_input("\x1b[<32;1;1M"));
    (void)tui.render();
    scheduler.run_intervals(1);
    (void)tui.render();
    const auto top_at_stop = tui.viewport_top();
    REQUIRE(tui.stop());
    const auto runs_at_stop = scheduler.interval_runs_;
    scheduler.run_intervals(3);
    CHECK(scheduler.interval_runs_ == runs_at_stop + 3);
    // stop() is final: no queued tick changes the state it left behind
    // (the frozen counter proof above shows the interval no longer runs).
    CHECK(top_at_stop < 6);
}

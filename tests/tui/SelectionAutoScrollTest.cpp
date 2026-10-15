// Selection auto-scroll replay (#1008). Every edge, cancellation, limit,
// nested-viewport and content-growth expectation is read from the verified
// pi-v1.0.4 bundle. The manual scheduler makes each frozen interval tick
// directly observable without a wall-clock assertion.

#include <cch/tui/AltScreen.hpp>
#include <cch/tui/Mouse.hpp>
#include <cch/tui/ScrollView.hpp>
#include <cch/tui/Stack.hpp>
#include <cch/tui/Text.hpp>
#include <cch/tui/VirtualTerminal.hpp>
#include <cch/tui/VStack.hpp>

#include "support/PiTuiEvidence.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using cch::support::JsonValue;

class ManualDelayedHandle final : public cch::tui::TuiScheduledHandle {
public:
    ~ManualDelayedHandle() override { *cancelled_ = true; }
    explicit ManualDelayedHandle(std::shared_ptr<bool> cancelled) : cancelled_(std::move(cancelled)) {}
    void cancel() noexcept override { *cancelled_ = true; }

private:
    std::shared_ptr<bool> cancelled_;
};

class ManualScheduler final : public cch::tui::TuiScheduler {
public:
    [[nodiscard]] std::chrono::steady_clock::time_point now() const noexcept override {
        return std::chrono::steady_clock::now();
    }

    void post(cch::tui::TuiScheduledWork work) override { posted_.push_back(std::move(work)); }

    void schedule(std::chrono::steady_clock::duration, cch::tui::TuiScheduledWork work) override {
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

    [[nodiscard]] std::shared_ptr<cch::tui::TuiScheduledWork> snapshot_interval() const { return interval_work_; }

    void run_interval_ticks(std::size_t count) {
        for (std::size_t tick = 0; tick < count; ++tick) {
            const auto work = interval_work_;
            if (!work || !*work) break;
            ++interval_runs_;
            (*work)();
        }
    }

    void run_scheduled() {
        auto work = std::exchange(scheduled_, nullptr);
        if (work) work();
    }

    void advance(std::chrono::steady_clock::duration duration) {
        now_ += duration;
        for (auto& item : delayed_) {
            if (!*item.cancelled && item.deadline <= now_) {
                *item.cancelled = true;
                if (item.work) item.work();
            }
        }
    }

    [[nodiscard]] bool has_scheduled() const noexcept { return static_cast<bool>(scheduled_); }
    [[nodiscard]] std::size_t interval_runs() const noexcept { return interval_runs_; }
    [[nodiscard]] std::size_t schedule_count() const noexcept { return schedule_count_; }

private:
    struct Delayed {
        std::chrono::steady_clock::time_point deadline{};
        cch::tui::TuiScheduledWork work;
        std::shared_ptr<bool> cancelled;
    };
    cch::tui::TuiScheduledWork scheduled_;
    std::shared_ptr<cch::tui::TuiScheduledWork> interval_work_;
    std::vector<cch::tui::TuiScheduledWork> posted_;
    std::vector<Delayed> delayed_;
    std::chrono::steady_clock::time_point now_{};
    std::size_t interval_runs_{0};
    std::size_t schedule_count_{0};
};

[[nodiscard]] cch::support::Expected<JsonValue> named_bundle() {
    return cch::tests::read_pi_tui_evidence("scroll-view.json");
}

[[nodiscard]] const JsonValue& scenario_of(const JsonValue& bundle, std::string_view name) {
    for (const auto& scenario : bundle.at("scenarios").get_array()) {
        if (scenario.at("name").get_string() == name) return scenario;
    }
    REQUIRE(false);
    return bundle;
}

[[nodiscard]] std::ptrdiff_t int_of(const JsonValue& value) { return static_cast<std::ptrdiff_t>(value.get_number()); }

[[nodiscard]] std::string document_lines(std::size_t count) {
    std::string text;
    for (std::size_t index = 1; index <= count; ++index) {
        if (index > 1) text += '\n';
        text += "line " + std::to_string(index);
    }
    return text;
}

[[nodiscard]] std::string numbered_lines(std::size_t count) {
    std::string text;
    for (std::size_t index = 1; index <= count; ++index) {
        if (index > 1) text += '\n';
        text += std::to_string(index);
    }
    return text;
}

[[nodiscard]] std::string osc52(std::string_view text) {
    constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string encoded;
    encoded.reserve((text.size() + 2) / 3 * 4);
    for (std::size_t index = 0; index < text.size(); index += 3) {
        const auto first = static_cast<unsigned char>(text[index]);
        const auto second = index + 1 < text.size() ? static_cast<unsigned char>(text[index + 1]) : 0;
        const auto third = index + 2 < text.size() ? static_cast<unsigned char>(text[index + 2]) : 0;
        encoded.push_back(alphabet[first >> 2]);
        encoded.push_back(alphabet[((first & 0x03) << 4) | (second >> 4)]);
        encoded.push_back(index + 1 < text.size() ? alphabet[((second & 0x0f) << 2) | (third >> 6)] : '=');
        encoded.push_back(index + 2 < text.size() ? alphabet[third & 0x3f] : '=');
    }
    return std::format("\x1b]52;c;{}\x07", encoded);
}

[[nodiscard]] std::string mouse_report(const JsonValue& point, std::string_view button, bool release = false) {
    return std::format("\x1b[<{};{};{}{}", button, int_of(point.at("x")), int_of(point.at("y")), release ? 'm' : 'M');
}

void inject_mouse(
        cch::tui::VirtualTerminal& terminal, const JsonValue& point, std::string_view button, bool release = false) {
    REQUIRE(terminal.inject_input(mouse_report(point, button, release)));
}

void observe_clipboard(cch::tui::AltScreen& tui, std::vector<std::string>& writes) {
    tui.set_selection_copy_sink([&writes](std::string_view text) -> cch::support::ExpectedVoid {
        writes.push_back(osc52(text));
        return {};
    });
}

[[nodiscard]] std::vector<std::string> expected_strings(const JsonValue& values) {
    std::vector<std::string> result;
    for (const auto& value : values.get_array())
        result.push_back(value.get_string());
    return result;
}

void check_optional_coordinate(const std::optional<std::pair<std::ptrdiff_t, std::ptrdiff_t>>& actual,
        const JsonValue& row_key,
        const JsonValue& column_key) {
    const bool expected = !row_key.holds<JsonValue::null_t>() && !column_key.holds<JsonValue::null_t>();
    CHECK(actual.has_value() == expected);
    if (expected && actual) {
        CHECK(actual->first == int_of(row_key));
        CHECK(actual->second == int_of(column_key));
    }
}

void check_selection_text(cch::tui::AltScreen& tui, const JsonValue& expected) {
    const auto actual = tui.selection_text_trace();
    if (expected.holds<JsonValue::null_t>()) {
        CHECK_FALSE(actual.has_value());
    } else {
        REQUIRE(actual.has_value());
        CHECK(*actual == expected.get_string());
    }
}

void check_implicit_trace(cch::tui::AltScreen& tui, const JsonValue& trace) {
    CHECK(static_cast<std::ptrdiff_t>(tui.viewport_top()) == int_of(trace.at("viewportTop")));
    check_optional_coordinate(tui.selection_anchor_trace(), trace.at("anchorRow"), trace.at("anchorCol"));
    check_optional_coordinate(tui.selection_focus_trace(), trace.at("focusRow"), trace.at("focusCol"));
    check_selection_text(tui, trace.at("selectionText"));
}

void run_implicit_trace(cch::tui::AltScreen& tui, ManualScheduler& scheduler, const JsonValue& trace) {
    scheduler.run_interval_ticks(1);
    scheduler.run_scheduled();
    check_implicit_trace(tui, trace);
}

} // namespace

TEST_CASE("AltScreen replays frozen edge auto-scroll ticks and OSC 52 copy bytes",
        "[tui][alt-screen][selection][issue1008][compat-pi]") {
    const auto bundle = named_bundle();
    REQUIRE(bundle);
    const auto& scenario = scenario_of(*bundle, "alt-screen-auto-scroll-edge");
    const auto& inputs = scenario.at("inputs");
    const auto& expected = scenario.at("expected");
    const auto& traces = expected.at("trace").get_array();

    cch::tui::VirtualTerminal terminal(
            {.columns = static_cast<std::size_t>(int_of(scenario.at("dimensions").at("columns"))),
                    .rows = static_cast<std::size_t>(int_of(scenario.at("dimensions").at("rows")))});
    cch::tui::AltScreen tui(terminal);
    ManualScheduler scheduler;
    tui.set_scheduler(&scheduler);
    std::vector<std::string> writes;
    observe_clipboard(tui, writes);
    REQUIRE(tui.add_child(std::make_unique<cch::tui::Text>(
            document_lines(static_cast<std::size_t>(int_of(inputs.at("documentLines")))), 0, 0)));
    REQUIRE(tui.start());
    REQUIRE(tui.render());
    CHECK(static_cast<std::ptrdiff_t>(tui.viewport_top()) == int_of(expected.at("initialViewportTop")));

    inject_mouse(terminal, inputs.at("press"), "0");
    REQUIRE(tui.render());
    inject_mouse(terminal, inputs.at("drag"), "32");
    REQUIRE(tui.render());
    REQUIRE(traces.size() == static_cast<std::size_t>(int_of(expected.at("tickCount"))));
    for (const auto& trace : traces)
        run_implicit_trace(tui, scheduler, trace);

    inject_mouse(terminal, inputs.at("release"), "0", true);
    REQUIRE(tui.render());
    CHECK(writes == expected_strings(expected.at("clipboardWrites")));
    REQUIRE(tui.stop());
}

TEST_CASE("AltScreen replays frozen release and direction cancellation traces",
        "[tui][alt-screen][selection][issue1008][compat-pi]") {
    const auto bundle = named_bundle();
    REQUIRE(bundle);
    const auto& scenario = scenario_of(*bundle, "alt-screen-auto-scroll-cancel");
    const auto& inputs = scenario.at("inputs");
    const auto& expected = scenario.at("expected");
    const auto& traces = expected.at("trace").get_array();

    cch::tui::VirtualTerminal terminal({.columns = 20, .rows = 4});
    cch::tui::AltScreen tui(terminal);
    ManualScheduler scheduler;
    tui.set_scheduler(&scheduler);
    std::vector<std::string> writes;
    observe_clipboard(tui, writes);
    REQUIRE(tui.add_child(std::make_unique<cch::tui::Text>(document_lines(10), 0, 0)));
    REQUIRE(tui.start());
    REQUIRE(tui.render());
    CHECK(static_cast<std::ptrdiff_t>(tui.viewport_top()) == int_of(expected.at("initialViewportTop")));

    inject_mouse(terminal, inputs.at("press"), "0");
    REQUIRE(tui.render());
    inject_mouse(terminal, inputs.at("firstDrag"), "32");
    REQUIRE(tui.render());
    REQUIRE(traces.size() == 4);
    run_implicit_trace(tui, scheduler, traces[0]);
    run_implicit_trace(tui, scheduler, traces[1]);
    CHECK(static_cast<std::ptrdiff_t>(scheduler.interval_runs()) == int_of(expected.at("tickCount")));

    inject_mouse(terminal, inputs.at("release"), "0", true);
    REQUIRE(tui.render());
    check_implicit_trace(tui, traces[2]);
    scheduler.run_interval_ticks(1);
    CHECK(static_cast<std::ptrdiff_t>(scheduler.interval_runs()) == int_of(expected.at("tickCount")));

    inject_mouse(terminal, inputs.at("secondPress"), "0");
    REQUIRE(tui.render());
    inject_mouse(terminal, inputs.at("secondDrag"), "32");
    REQUIRE(tui.render());
    check_implicit_trace(tui, traces[3]);
    CHECK(static_cast<std::ptrdiff_t>(scheduler.interval_runs()) == int_of(expected.at("tickCount")));
    inject_mouse(terminal, inputs.at("secondRelease"), "0", true);
    REQUIRE(tui.render());
    CHECK(writes == expected_strings(expected.at("clipboardWrites")));
    REQUIRE(tui.stop());
}

TEST_CASE("AltScreen replays frozen auto-scroll top and bottom limits",
        "[tui][alt-screen][selection][issue1008][compat-pi]") {
    const auto bundle = named_bundle();
    REQUIRE(bundle);
    const auto& scenario = scenario_of(*bundle, "alt-screen-auto-scroll-limit");
    const auto& inputs = scenario.at("inputs");
    const auto& expected = scenario.at("expected");

    const auto replay = [&](const JsonValue& setup, const JsonValue& frozen, bool bottom_limit) {
        const auto& traces = frozen.at("trace").get_array();
        cch::tui::VirtualTerminal terminal({.columns = 20, .rows = 4});
        cch::tui::AltScreen tui(terminal);
        ManualScheduler scheduler;
        tui.set_scheduler(&scheduler);
        std::vector<std::string> writes;
        observe_clipboard(tui, writes);
        REQUIRE(tui.add_child(std::make_unique<cch::tui::Text>(
                document_lines(static_cast<std::size_t>(int_of(setup.at("documentLines")))), 0, 0)));
        REQUIRE(tui.start());
        REQUIRE(tui.render());
        if (bottom_limit) {
            tui.scroll_by(int_of(setup.at("manualScrollDelta")));
            REQUIRE(tui.render());
        } else if (setup.at("scrollToTop").get_boolean()) {
            tui.scroll_to_top();
            REQUIRE(tui.render());
        }
        CHECK(static_cast<std::ptrdiff_t>(tui.viewport_top()) == int_of(frozen.at("initialViewportTop")));
        inject_mouse(terminal, setup.at("press"), "0");
        REQUIRE(tui.render());
        inject_mouse(terminal, setup.at("drag"), "32");
        REQUIRE(tui.render());
        CHECK(traces.size() == static_cast<std::size_t>(int_of(frozen.at("tickCount"))));
        for (const auto& trace : traces)
            run_implicit_trace(tui, scheduler, trace);
        inject_mouse(terminal, setup.at("release"), "0", true);
        REQUIRE(tui.render());
        CHECK(writes == expected_strings(frozen.at("clipboardWrites")));
        REQUIRE(tui.stop());
    };

    replay(inputs, expected, false);
    const auto& bottom = scenario_of(*bundle, "alt-screen-auto-scroll-limit-bottom");
    replay(bottom.at("inputs"), bottom.at("expected"), true);
}

TEST_CASE("AltScreen auto-scroll ticks follow the selection anchor viewport in a nested layout",
        "[tui][alt-screen][selection][issue1008][compat-pi]") {
    const auto bundle = named_bundle();
    REQUIRE(bundle);
    const auto& scenario = scenario_of(*bundle, "alt-screen-auto-scroll-nested");
    const auto& inputs = scenario.at("inputs");
    const auto& expected = scenario.at("expected");
    const auto& traces = expected.at("trace").get_array();

    cch::tui::VirtualTerminal terminal({.columns = 20, .rows = 4});
    cch::tui::AltScreen tui(terminal);
    ManualScheduler scheduler;
    tui.set_scheduler(&scheduler);
    std::vector<std::string> writes;
    observe_clipboard(tui, writes);
    auto inner = std::make_unique<cch::tui::ScrollView>(std::make_unique<cch::tui::Text>(
            numbered_lines(static_cast<std::size_t>(int_of(inputs.at("innerLines")))), 0, 0));
    auto* inner_ptr = inner.get();
    cch::tui::VStack content;
    REQUIRE(content.add_child(std::make_unique<cch::tui::Text>(inputs.at("head").get_string(), 0, 0),
            cch::tui::StackEntryOptions{.basis = static_cast<std::size_t>(int_of(inputs.at("headRows")))}));
    REQUIRE(content.add_child(std::move(inner),
            cch::tui::StackEntryOptions{.basis = static_cast<std::size_t>(int_of(inputs.at("innerRows")))}));
    REQUIRE(content.add_child(std::make_unique<cch::tui::Text>(inputs.at("tail").get_string(), 0, 0),
            cch::tui::StackEntryOptions{.basis = static_cast<std::size_t>(int_of(inputs.at("tailRows")))}));
    auto outer = std::make_unique<cch::tui::ScrollView>(std::make_unique<cch::tui::VStack>(std::move(content)),
            cch::tui::ScrollViewOptions{.follow_end = true, .primary = true});
    auto* outer_ptr = outer.get();
    tui.set_layout_root(std::move(outer));
    REQUIRE(tui.start());
    REQUIRE(tui.render());
    CHECK(static_cast<std::ptrdiff_t>(outer_ptr->scroll_top()) == int_of(expected.at("initialOuterScrollTop")));
    CHECK(static_cast<std::ptrdiff_t>(inner_ptr->scroll_top()) == int_of(expected.at("initialInnerScrollTop")));

    inject_mouse(terminal, inputs.at("press"), "0");
    REQUIRE(tui.render());
    inject_mouse(terminal, inputs.at("drag"), "32");
    REQUIRE(tui.render());
    REQUIRE(traces.size() == static_cast<std::size_t>(int_of(expected.at("tickCount"))));
    for (const auto& trace : traces) {
        scheduler.run_interval_ticks(1);
        scheduler.run_scheduled();
        CHECK(static_cast<std::ptrdiff_t>(inner_ptr->scroll_top()) == int_of(trace.at("innerScrollTop")));
        CHECK(static_cast<std::ptrdiff_t>(outer_ptr->scroll_top()) == int_of(trace.at("outerScrollTop")));
        CHECK(trace.at("scrolledView").get_string() == "inner");
        check_optional_coordinate(tui.selection_anchor_trace(), trace.at("anchorRow"), trace.at("anchorCol"));
        check_optional_coordinate(tui.selection_focus_trace(), trace.at("focusRow"), trace.at("focusCol"));
        check_selection_text(tui, trace.at("selectionText"));
    }
    inject_mouse(terminal, inputs.at("release"), "0", true);
    REQUIRE(tui.render());
    CHECK(writes == expected_strings(expected.at("clipboardWrites")));
    REQUIRE(tui.stop());
}

TEST_CASE("AltScreen keeps selection coordinates valid as auto-scrolled content grows",
        "[tui][alt-screen][selection][issue1008][compat-pi]") {
    const auto bundle = named_bundle();
    REQUIRE(bundle);
    const auto& scenario = scenario_of(*bundle, "alt-screen-auto-scroll-content-growth");
    const auto& inputs = scenario.at("inputs");
    const auto& expected = scenario.at("expected");
    const auto& traces = expected.at("trace").get_array();

    cch::tui::VirtualTerminal terminal({.columns = 20, .rows = 4});
    cch::tui::AltScreen tui(terminal);
    ManualScheduler scheduler;
    tui.set_scheduler(&scheduler);
    std::vector<std::string> writes;
    observe_clipboard(tui, writes);
    auto text = std::make_unique<cch::tui::Text>(
            numbered_lines(static_cast<std::size_t>(int_of(inputs.at("documentLines")))), 0, 0);
    auto* text_ptr = text.get();
    REQUIRE(tui.add_child(std::move(text)));
    REQUIRE(tui.start());
    REQUIRE(tui.render());
    tui.scroll_by(int_of(inputs.at("manualScrollDelta")));
    REQUIRE(tui.render());
    CHECK(static_cast<std::ptrdiff_t>(tui.viewport_top()) == int_of(expected.at("initialViewportTop")));

    inject_mouse(terminal, inputs.at("press"), "0");
    REQUIRE(tui.render());
    inject_mouse(terminal, inputs.at("drag"), "32");
    REQUIRE(tui.render());
    const auto growth_tick = static_cast<std::size_t>(int_of(inputs.at("growAfterTick")));
    REQUIRE(traces.size() == growth_tick + static_cast<std::size_t>(int_of(expected.at("ticksAfterGrowth"))));
    for (std::size_t index = 0; index < growth_tick; ++index)
        run_implicit_trace(tui, scheduler, traces[index]);

    check_implicit_trace(tui, expected.at("growthState"));
    text_ptr->set_text(numbered_lines(static_cast<std::size_t>(int_of(inputs.at("grownDocumentLines")))));
    tui.request_render();
    scheduler.run_scheduled();
    check_implicit_trace(tui, expected.at("afterGrowth"));
    for (std::size_t index = growth_tick; index < traces.size(); ++index)
        run_implicit_trace(tui, scheduler, traces[index]);

    inject_mouse(terminal, inputs.at("release"), "0", true);
    REQUIRE(tui.render());
    CHECK(writes == expected_strings(expected.at("clipboardWrites")));
    REQUIRE(tui.stop());
}

TEST_CASE("AltScreen discards a queued auto-scroll tick after a new selection is armed",
        "[tui][alt-screen][selection][issue1008]") {
    cch::tui::VirtualTerminal terminal({.columns = 20, .rows = 4});
    cch::tui::AltScreen tui(terminal);
    ManualScheduler scheduler;
    tui.set_scheduler(&scheduler);
    REQUIRE(tui.add_child(std::make_unique<cch::tui::Text>(document_lines(16), 0, 0)));
    tui.set_copy_on_select(false);
    REQUIRE(tui.start());
    REQUIRE(tui.render());

    REQUIRE(terminal.inject_input("\x1b[<0;1;4M"));
    REQUIRE(tui.render());
    REQUIRE(terminal.inject_input("\x1b[<32;1;1M"));
    REQUIRE(tui.render());
    const auto stale = scheduler.snapshot_interval();
    REQUIRE(stale);
    REQUIRE(*stale);
    (*stale)();
    scheduler.run_scheduled();
    REQUIRE(tui.viewport_top() == 11);

    REQUIRE(terminal.inject_input("\x1b[<0;1;1m"));
    REQUIRE(tui.render());
    tui.scroll_by(-7);
    REQUIRE(tui.render());
    CHECK(tui.viewport_top() == 4);

    REQUIRE(terminal.inject_input("\x1b[<0;1;1M"));
    REQUIRE(tui.render());
    REQUIRE(terminal.inject_input("\x1b[<32;1;1M"));
    REQUIRE(tui.render());
    const auto before_top = tui.viewport_top();
    const auto before_anchor = tui.selection_anchor_trace();
    const auto before_focus = tui.selection_focus_trace();
    REQUIRE(before_anchor);
    REQUIRE(before_focus);
    CHECK_FALSE(scheduler.has_scheduled());
    const auto before_schedule_count = scheduler.schedule_count();

    (*stale)();
    CHECK(scheduler.schedule_count() == before_schedule_count);
    scheduler.run_scheduled();
    CHECK(tui.viewport_top() == before_top);
    CHECK(tui.selection_anchor_trace() == before_anchor);
    CHECK(tui.selection_focus_trace() == before_focus);
    CHECK_FALSE(scheduler.has_scheduled());
    REQUIRE(tui.stop());
}

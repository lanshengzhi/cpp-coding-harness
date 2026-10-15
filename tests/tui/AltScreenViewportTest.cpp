#include <cch/tui/AltScreen.hpp>
#include <cch/tui/HStack.hpp>
#include <cch/tui/Keybindings.hpp>
#include <cch/tui/MouseRegion.hpp>
#include <cch/tui/ScrollView.hpp>
#include <cch/tui/Text.hpp>
#include <cch/tui/VStack.hpp>
#include <cch/tui/VirtualTerminal.hpp>

#include "support/FullscreenKeybindings.hpp"
#include "support/PiTuiEvidence.hpp"

using cch::tests::fullscreen_registry;
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using cch::support::JsonValue;

[[nodiscard]] cch::support::Expected<JsonValue> nav_evidence() {
    return cch::tests::read_pi_tui_evidence("alt-screen-nav.json");
}

[[nodiscard]] const JsonValue& nav_scenario_of(const JsonValue& bundle, std::string_view name) {
    for (const auto& scenario : bundle.at("scenarios").get_array()) {
        if (scenario.at("name").get_string() == name) return scenario;
    }
    REQUIRE(false);
    return bundle;
}

[[nodiscard]] std::vector<std::string> nav_lines_of(const JsonValue& value) {
    std::vector<std::string> lines;
    for (const auto& line : value.get_array())
        lines.push_back(line.get_string());
    return lines;
}

[[nodiscard]] std::string numbered_lines(std::size_t count) {
    std::string result;
    for (std::size_t index = 1; index <= count; ++index) {
        if (index > 1) result.push_back('\n');
        result += "line " + std::to_string(index);
    }
    return result;
}

class FocusProbe final : public cch::tui::Component, public cch::tui::Focusable, public cch::tui::InputHandler {
public:
    [[nodiscard]] cch::support::Expected<cch::tui::RenderResult> render(std::size_t) override {
        return cch::tui::RenderResult{.lines = {"editor"}};
    }

    void invalidate() override {}

    cch::tui::InputAdmissionOutcome handle_input(const cch::tui::InputEventVariant& event) override {
        if (std::holds_alternative<cch::tui::KeyEvent>(event)) ++keys_received;
        return cch::tui::InputAdmissionOutcome::Consumed;
    }

    void set_focused(bool focused) override { focused_ = focused; }
    [[nodiscard]] bool focused() const override { return focused_; }

    std::size_t keys_received{0};

private:
    bool focused_{false};
};

} // namespace

TEST_CASE("AltScreen navigates raw OSC 133 prompt rows and consumes releases", "[tui][alt-screen][issue1011]") {
    const auto bundle = nav_evidence();
    REQUIRE(bundle);
    const auto& scenario = nav_scenario_of(*bundle, "prompt-navigation-uses-raw-osc-133-and-ignores-releases");
    const auto& navigation = scenario.at("expected").at("navigation").get_array();
    REQUIRE(navigation.size() == 6);
    const auto prompt = [](std::string_view terminator, int message) {
        return std::string{"\x1b]133;A"} + std::string{terminator} + "message " + std::to_string(message) + "\ndetail";
    };
    const auto transcript =
            prompt("\x07", 1) + "\n" + prompt("\x1b\\", 2) + "\n" + prompt("\x07", 3) + "\n" + prompt("\x07", 4);
    cch::tui::VirtualTerminal terminal({.columns = 24, .rows = 3});
    cch::tui::AltScreen tui(terminal);
    REQUIRE(tui.add_child(std::make_unique<cch::tui::Text>(transcript, 0, 0)));
    tui.set_keybinding_context(std::make_shared<cch::tui::KeybindingsContext>(fullscreen_registry()));
    REQUIRE(tui.start());
    REQUIRE(tui.render());
    const auto check_navigation = [&](std::size_t index) {
        const auto& expected = navigation[index];
        CHECK(tui.viewport_top() == static_cast<std::size_t>(expected.at("viewportTop").get_number()));
        auto screen = terminal.screen();
        for (auto& line : screen) {
            while (!line.empty() && line.back() == ' ')
                line.pop_back();
        }
        CHECK(screen == nav_lines_of(expected.at("screen")));
    };
    check_navigation(0);

    REQUIRE(terminal.inject_input("\x1b[57419;6u"));
    REQUIRE(tui.render());
    check_navigation(1);
    REQUIRE(terminal.inject_input("\x1b[57419;6:3u"));
    REQUIRE(tui.render());
    CHECK(tui.viewport_top() == static_cast<std::size_t>(navigation[1].at("viewportTop").get_number()));

    REQUIRE(terminal.inject_input("\x1b[1;6A"));
    REQUIRE(tui.render());
    check_navigation(2);
    REQUIRE(terminal.inject_input("\x1b[1;6A"));
    REQUIRE(tui.render());
    check_navigation(3);

    REQUIRE(terminal.inject_input("\x1b[57420;6u"));
    REQUIRE(tui.render());
    check_navigation(4);
    REQUIRE(terminal.inject_input("\x1b[57420;6:3u"));
    REQUIRE(tui.render());
    CHECK(tui.viewport_top() == static_cast<std::size_t>(navigation[4].at("viewportTop").get_number()));
    REQUIRE(terminal.inject_input("\x1b[1;6B"));
    REQUIRE(tui.render());
    check_navigation(5);
    REQUIRE(tui.stop());

    const auto& no_prompt_scenario = nav_scenario_of(*bundle, "prompt-navigation-leaves-view-when-no-prompt-matches");
    const auto& no_prompt_navigation = no_prompt_scenario.at("expected").at("navigation").get_array();
    REQUIRE(no_prompt_navigation.size() == 3);
    cch::tui::VirtualTerminal no_prompt_terminal({.columns = 20, .rows = 3});
    cch::tui::AltScreen no_prompt_tui(no_prompt_terminal);
    REQUIRE(no_prompt_tui.add_child(
            std::make_unique<cch::tui::Text>("\x1b]133;B\x07"
                                             "message output\n"
                                             "\x1b]133;C\x07"
                                             "assistant output\nplain three\nplain four\nplain five",
                    0,
                    0)));
    no_prompt_tui.set_keybinding_context(std::make_shared<cch::tui::KeybindingsContext>(fullscreen_registry()));
    REQUIRE(no_prompt_tui.start());
    const auto no_prompt_initial = no_prompt_tui.render();
    REQUIRE(no_prompt_initial.has_value());
    for (std::size_t index = 0; index < 2; ++index) {
        REQUIRE(no_prompt_terminal.inject_input(index == 0 ? "\x1b[1;6A" : "\x1b[1;6B"));
        REQUIRE(no_prompt_tui.render());
        CHECK(no_prompt_tui.viewport_top() ==
                static_cast<std::size_t>(no_prompt_navigation[index + 1].at("viewportTop").get_number()));
        auto screen = no_prompt_terminal.screen();
        for (auto& line : screen) {
            while (!line.empty() && line.back() == ' ')
                line.pop_back();
        }
        CHECK(screen == nav_lines_of(no_prompt_navigation[index + 1].at("screen")));
    }
    REQUIRE(no_prompt_tui.stop());
}

TEST_CASE("AltScreen flash replacement matches frozen top-row replay", "[tui][alt-screen][issue1011][compat-pi]") {
    const auto bundle = nav_evidence();
    REQUIRE(bundle);
    const auto& scenario = nav_scenario_of(*bundle, "flash-replaces-top-rows-oldest-first");
    const auto& dimensions = scenario.at("dimensions");
    const auto& inputs = scenario.at("inputs");
    cch::tui::VirtualTerminal terminal({.columns = static_cast<std::size_t>(dimensions.at("columns").get_number()),
            .rows = static_cast<std::size_t>(dimensions.at("rows").get_number())});
    cch::tui::AltScreen tui(terminal);
    std::string transcript;
    for (const auto& line : inputs.at("transcript").get_array()) {
        if (!transcript.empty()) transcript.push_back('\n');
        transcript += line.get_string();
    }
    REQUIRE(tui.add_child(std::make_unique<cch::tui::Text>(std::move(transcript), 0, 0)));
    REQUIRE(tui.start());
    REQUIRE(tui.render());
    const auto& flashes = inputs.at("flashes").get_array();
    REQUIRE(flashes.size() == 2);
    tui.flash(flashes[0].get_string(), std::chrono::seconds{5});
    tui.flash(flashes[1].get_string(), std::chrono::seconds{5});
    REQUIRE(tui.render());
    const auto& expected = scenario.at("expected").at("screen");
    auto lines = terminal.screen();
    for (auto& line : lines) {
        while (!line.empty() && line.back() == ' ')
            line.pop_back();
    }
    REQUIRE(lines.size() == expected.get_array().size());
    const auto expected_lines = nav_lines_of(expected);
    REQUIRE(lines.size() == expected_lines.size());
    for (std::size_t row = 0; row < lines.size(); ++row) {
        CHECK(lines[row] == expected_lines[row]);
    }
    CHECK(lines[0].find("First") != std::string::npos);
    CHECK(lines[1].find("Second") != std::string::npos);
    CHECK(lines[2].find("three") != std::string::npos);
    REQUIRE(tui.stop());

    const auto& overflow_scenario = nav_scenario_of(*bundle, "flash-overflow-keeps-newest-rows-in-original-order");
    cch::tui::VirtualTerminal overflow_terminal({.columns = 20, .rows = 4});
    cch::tui::AltScreen overflow_tui(overflow_terminal);
    REQUIRE(overflow_tui.add_child(std::make_unique<cch::tui::Text>("one\ntwo\nthree\nfour", 0, 0)));
    REQUIRE(overflow_tui.start());
    REQUIRE(overflow_tui.render());
    const auto& overflow_messages = overflow_scenario.at("inputs").at("flashes").get_array();
    REQUIRE(overflow_messages.size() == 6);
    for (const auto& message : overflow_messages)
        overflow_tui.flash(message.get_string(), std::chrono::seconds{5});
    REQUIRE(overflow_tui.render());
    auto overflow_screen = overflow_terminal.screen();
    for (auto& line : overflow_screen) {
        while (!line.empty() && line.back() == ' ')
            line.pop_back();
    }
    CHECK(overflow_screen == nav_lines_of(overflow_scenario.at("expected").at("screen")));
    REQUIRE(overflow_tui.stop());
}

TEST_CASE("AltScreen flash timers expire independently and dispose safely", "[tui][alt-screen][issue1011]") {
    class Handle final : public cch::tui::TuiScheduledHandle {
    public:
        explicit Handle(std::shared_ptr<bool> cancelled) : cancelled_(std::move(cancelled)) {}
        ~Handle() override { cancel(); }
        void cancel() noexcept override { *cancelled_ = true; }

    private:
        std::shared_ptr<bool> cancelled_;
    };
    class Scheduler final : public cch::tui::TuiScheduler {
    public:
        [[nodiscard]] std::chrono::steady_clock::time_point now() const noexcept override { return now_; }
        void post(cch::tui::TuiScheduledWork work) override { posted_.push_back(std::move(work)); }
        void schedule(std::chrono::steady_clock::duration, cch::tui::TuiScheduledWork work) override {
            scheduled_ = std::move(work);
        }
        [[nodiscard]] std::unique_ptr<cch::tui::TuiScheduledHandle> schedule_delayed(
                std::chrono::steady_clock::duration delay, cch::tui::TuiScheduledWork work) override {
            auto cancelled = std::make_shared<bool>(false);
            delayed_.push_back(Delayed{.deadline = now_ + delay, .work = std::move(work), .cancelled = cancelled});
            return std::make_unique<Handle>(std::move(cancelled));
        }
        void cancel() noexcept override { scheduled_ = nullptr; }
        void schedule_interval(std::chrono::steady_clock::duration, cch::tui::TuiScheduledWork work) override {
            interval_ = std::make_shared<cch::tui::TuiScheduledWork>(std::move(work));
        }
        void cancel_interval() noexcept override { interval_.reset(); }
        void advance(std::chrono::steady_clock::duration delta) {
            now_ += delta;
            for (auto& item : delayed_) {
                if (!*item.cancelled && item.deadline <= now_) {
                    *item.cancelled = true;
                    if (item.work) item.work();
                }
            }
        }
        [[nodiscard]] std::size_t pending_delayed() const {
            return static_cast<std::size_t>(
                    std::count_if(delayed_.begin(), delayed_.end(), [](const auto& item) { return !*item.cancelled; }));
        }
        void cancel_delayed() {
            for (auto& item : delayed_)
                *item.cancelled = true;
        }

    private:
        struct Delayed {
            std::chrono::steady_clock::time_point deadline{};
            cch::tui::TuiScheduledWork work;
            std::shared_ptr<bool> cancelled;
        };
        std::chrono::steady_clock::time_point now_{};
        cch::tui::TuiScheduledWork scheduled_;
        std::shared_ptr<cch::tui::TuiScheduledWork> interval_;
        std::vector<cch::tui::TuiScheduledWork> posted_;
        std::vector<Delayed> delayed_;
    };

    Scheduler scheduler;
    cch::tui::VirtualTerminal terminal({.columns = 20, .rows = 4});
    cch::tui::AltScreen tui(terminal);
    REQUIRE(tui.add_child(std::make_unique<cch::tui::Text>("one\ntwo\nthree\nfour", 0, 0)));
    tui.set_scheduler(&scheduler);
    REQUIRE(tui.start());
    REQUIRE(tui.render());

    tui.flash("First", std::chrono::milliseconds{80});
    tui.flash("Second", std::chrono::milliseconds{500});
    REQUIRE(tui.render());
    CHECK(terminal.screen()[0].ends_with(" First "));
    CHECK(terminal.screen()[1].ends_with(" Second "));

    scheduler.advance(std::chrono::milliseconds{100});
    REQUIRE(tui.render());
    CHECK(terminal.screen()[0].ends_with(" Second "));
    CHECK(std::none_of(terminal.screen().begin(), terminal.screen().end(), [](const auto& line) {
        return line.find("First") != std::string::npos;
    }));

    tui.flash("Disposed", std::chrono::seconds{10});
    REQUIRE(scheduler.pending_delayed() == 2);
    REQUIRE(tui.stop());
    CHECK(scheduler.pending_delayed() == 0);
    scheduler.advance(std::chrono::seconds{20});
    CHECK(terminal.screen()[0].find("Second") == std::string::npos);
}

TEST_CASE("AltScreen indicator follows follow-end gating and click-to-end", "[tui][alt-screen][issue1011]") {
    const auto bundle = nav_evidence();
    REQUIRE(bundle);
    const auto& scenario = nav_scenario_of(*bundle, "indicator-shown-when-scrolled");
    cch::tui::VirtualTerminal terminal({.columns = 30, .rows = 3});
    cch::tui::AltScreen tui(
            terminal, cch::tui::AltScreenOptions{.scroll_to_end_indicator = [] { return " ↓ Jump to end "; }});
    auto scroll = std::make_unique<cch::tui::ScrollView>(
            std::make_unique<cch::tui::Text>("one\ntwo\nthree\nfour\nfive", 0, 0),
            cch::tui::ScrollViewOptions{.follow_end = true, .primary = true});
    auto* scroll_view = scroll.get();
    tui.set_layout_root(std::move(scroll));
    REQUIRE(tui.start());
    REQUIRE(tui.render());
    CHECK(std::none_of(terminal.screen().begin(), terminal.screen().end(), [](const auto& line) {
        return line.find("Jump to end") != std::string::npos;
    }));
    REQUIRE(terminal.inject_input("\x1b[<64;1;1M"));
    REQUIRE(tui.render());
    CHECK_FALSE(scroll_view->following_end());
    auto screen = terminal.screen();
    for (auto& line : screen) {
        while (!line.empty() && line.back() == ' ')
            line.pop_back();
    }
    const auto expected_screen = nav_lines_of(scenario.at("expected").at("screen"));
    CHECK(screen == expected_screen);
    CHECK(screen[2].find(scenario.at("expected").at("indicator").get_string()) != std::string::npos);
    REQUIRE(terminal.inject_input("\x1b[<0;15;3M"));
    REQUIRE(terminal.inject_input("\x1b[<0;15;3m"));
    REQUIRE(tui.render());
    CHECK(scroll_view->following_end());
    REQUIRE(tui.stop());

    const auto& no_follow_scenario = nav_scenario_of(*bundle, "indicator-hidden-without-follow-end");
    CHECK_FALSE(no_follow_scenario.at("expected").at("indicatorShown").get_boolean());
    cch::tui::VirtualTerminal no_follow_terminal({.columns = 30, .rows = 3});
    cch::tui::AltScreen no_follow_tui(no_follow_terminal,
            cch::tui::AltScreenOptions{.scroll_to_end_indicator = [] { return " ↓ Jump to end "; }});
    auto no_follow = std::make_unique<cch::tui::ScrollView>(
            std::make_unique<cch::tui::Text>("one\ntwo\nthree\nfour\nfive", 0, 0),
            cch::tui::ScrollViewOptions{.primary = true});
    auto* no_follow_view = no_follow.get();
    no_follow_tui.set_layout_root(std::move(no_follow));
    REQUIRE(no_follow_tui.start());
    REQUIRE(no_follow_tui.render());
    REQUIRE(no_follow_terminal.inject_input("\x1b[<64;1;1M"));
    REQUIRE(no_follow_tui.render());
    CHECK_FALSE(no_follow_view->follow_end());
    CHECK(std::none_of(no_follow_terminal.screen().begin(), no_follow_terminal.screen().end(), [](const auto& line) {
        return line.find("Jump to end") != std::string::npos;
    }));
    REQUIRE(no_follow_tui.stop());
}

TEST_CASE("AltScreen wheel policy matches fixed counts, velocity acceleration, resets, and Alt multiplier",
        "[tui][alt-screen][scroll][issue1004]") {
    std::vector<std::ptrdiff_t> deltas;
    auto region = std::make_unique<cch::tui::MouseRegion>(std::make_unique<cch::tui::Text>(numbered_lines(12), 0, 0),
            [&deltas](const cch::tui::MouseEvent& event) -> std::optional<cch::tui::MouseEventResult> {
                if (event.type != cch::tui::MouseEventType::Wheel) return std::nullopt;
                deltas.push_back(event.wheel_delta.value_or(0));
                return cch::tui::MouseEventResult{.handled = true, .render = false};
            });
    cch::tui::VirtualTerminal terminal({.columns = 24, .rows = 4});
    cch::tui::AltScreen tui(terminal, cch::tui::AltScreenOptions{.wheel_scroll_lines = std::size_t{3}});
    REQUIRE(tui.add_child(std::move(region)));
    REQUIRE(tui.start());
    REQUIRE(tui.render());

    REQUIRE(terminal.inject_input("\x1b[<65;1;1M"));
    REQUIRE(terminal.inject_input("\x1b[<73;1;1M"));
    CHECK(deltas == std::vector<std::ptrdiff_t>{3, 15});

    auto now = std::chrono::steady_clock::time_point{};
    tui.set_wheel_scroll_lines(cch::tui::AutoWheelScrollLines{});
    // `wheel_clock` is selected at construction, so build a second host with
    // the deterministic input stream for the auto-policy observations.
    REQUIRE(tui.stop());

    cch::tui::VirtualTerminal auto_terminal({.columns = 24, .rows = 4});
    cch::tui::AltScreen auto_tui(auto_terminal,
            cch::tui::AltScreenOptions{
                    .wheel_scroll_lines = cch::tui::AutoWheelScrollLines{}, .wheel_clock = [&now] { return now; }});
    deltas.clear();
    auto auto_region =
            std::make_unique<cch::tui::MouseRegion>(std::make_unique<cch::tui::Text>(numbered_lines(12), 0, 0),
                    [&deltas](const cch::tui::MouseEvent& event) -> std::optional<cch::tui::MouseEventResult> {
                        if (event.type != cch::tui::MouseEventType::Wheel) return std::nullopt;
                        deltas.push_back(event.wheel_delta.value_or(0));
                        return cch::tui::MouseEventResult{.handled = true, .render = false};
                    });
    REQUIRE(auto_tui.add_child(std::move(auto_region)));
    REQUIRE(auto_tui.start());
    REQUIRE(auto_tui.render());
    const auto send = [&](std::chrono::milliseconds advance, int button = 65) {
        now += advance;
        return auto_terminal.inject_input("\x1b[<" + std::to_string(button) + ";1;1M");
    };
    REQUIRE(send(std::chrono::milliseconds{0}));
    REQUIRE(send(std::chrono::milliseconds{50}));
    REQUIRE(send(std::chrono::milliseconds{50}));
    REQUIRE(send(std::chrono::milliseconds{3}));
    REQUIRE(send(std::chrono::milliseconds{20}, 64));
    REQUIRE(send(std::chrono::milliseconds{250}));
    CHECK(deltas == std::vector<std::ptrdiff_t>{1, 2, 2, 1, -1, 1});
    REQUIRE(auto_tui.stop());
}

TEST_CASE("AltScreen chains a nested wheel remainder to its parent without moving a sibling",
        "[tui][alt-screen][scroll][issue1004]") {
    cch::tui::VirtualTerminal terminal({.columns = 30, .rows = 3});
    cch::tui::AltScreen tui(terminal, cch::tui::AltScreenOptions{.wheel_scroll_lines = std::size_t{5}});

    auto inner = std::make_unique<cch::tui::ScrollView>(std::make_unique<cch::tui::Text>(numbered_lines(4), 0, 0));
    auto* inner_pointer = inner.get();
    cch::tui::VStack left_content;
    REQUIRE(left_content.add_child(
            std::make_unique<cch::tui::Text>("head", 0, 0), cch::tui::StackEntryOptions{.basis = 1}));
    REQUIRE(left_content.add_child(std::move(inner), cch::tui::StackEntryOptions{.basis = 2}));
    REQUIRE(left_content.add_child(
            std::make_unique<cch::tui::Text>("tail", 0, 0), cch::tui::StackEntryOptions{.basis = 5}));
    auto outer = std::make_unique<cch::tui::ScrollView>(
            std::make_unique<cch::tui::VStack>(std::move(left_content)), cch::tui::ScrollViewOptions{.primary = true});
    auto* outer_pointer = outer.get();
    auto unrelated = std::make_unique<cch::tui::ScrollView>(std::make_unique<cch::tui::Text>(numbered_lines(10), 0, 0));
    auto* unrelated_pointer = unrelated.get();
    cch::tui::HStack root;
    REQUIRE(root.add_child(std::move(outer), cch::tui::StackEntryOptions{.basis = 15}));
    REQUIRE(root.add_child(std::move(unrelated), cch::tui::StackEntryOptions{.basis = 15}));
    tui.set_layout_root(std::make_unique<cch::tui::HStack>(std::move(root)));
    REQUIRE(tui.start());
    REQUIRE(tui.render());

    CHECK(inner_pointer->scroll_top() == 0);
    CHECK(outer_pointer->scroll_top() == 0);
    CHECK(unrelated_pointer->scroll_top() == 0);
    REQUIRE(terminal.inject_input("\x1b[<65;2;2M"));
    REQUIRE(tui.render());
    CHECK(inner_pointer->scroll_top() == 2);
    CHECK(outer_pointer->scroll_top() == 3);
    CHECK(unrelated_pointer->scroll_top() == 0);
    REQUIRE(tui.stop());
}

TEST_CASE("AltScreen wheel over a plain region falls back to the primary viewport without moving an unrelated view",
        "[tui][alt-screen][scroll][issue1004]") {
    cch::tui::VirtualTerminal terminal({.columns = 30, .rows = 3});
    cch::tui::AltScreen tui(terminal, cch::tui::AltScreenOptions{.wheel_scroll_lines = std::size_t{5}});

    auto primary = std::make_unique<cch::tui::ScrollView>(
            std::make_unique<cch::tui::Text>(numbered_lines(6), 0, 0), cch::tui::ScrollViewOptions{.primary = true});
    auto* primary_pointer = primary.get();
    auto unrelated = std::make_unique<cch::tui::ScrollView>(std::make_unique<cch::tui::Text>(numbered_lines(10), 0, 0));
    auto* unrelated_pointer = unrelated.get();
    cch::tui::HStack root;
    REQUIRE(root.add_child(std::move(primary), cch::tui::StackEntryOptions{.basis = 15}));
    // Plain non-scroll region: the pointer lands here on the second wheel.
    REQUIRE(root.add_child(std::make_unique<cch::tui::Text>("plain", 0, 0), cch::tui::StackEntryOptions{.basis = 15}));
    tui.set_layout_root(std::make_unique<cch::tui::HStack>(std::move(root)));
    REQUIRE(tui.start());
    REQUIRE(tui.render());

    CHECK(primary_pointer->scroll_top() == 0);
    CHECK(unrelated_pointer->scroll_top() == 0);
    // Wheel over the plain right-hand region; the primary viewport absorbs the
    // delta (pi `tui-alt-screen.ts:993-995`).
    REQUIRE(terminal.inject_input("\x1b[<65;22;2M"));
    REQUIRE(tui.render());
    CHECK(primary_pointer->scroll_top() > 0);
    CHECK(unrelated_pointer->scroll_top() == 0);
    REQUIRE(tui.stop());
}

TEST_CASE("AltScreen routes primary navigation before focus and updates the clickable end indicator",
        "[tui][alt-screen][scroll][issue1004]") {
    cch::tui::VirtualTerminal terminal({.columns = 20,
            .rows = 4,
            .capabilities = {.synchronized_output = true, .keyboard_protocol = cch::tui::KeyboardProtocol::Kitty}});
    cch::tui::AltScreen tui(
            terminal, cch::tui::AltScreenOptions{.scroll_to_end_indicator = [] { return std::string{"Jump end"}; }});
    auto transcript = std::make_unique<cch::tui::ScrollView>(std::make_unique<cch::tui::Text>(numbered_lines(12), 0, 0),
            cch::tui::ScrollViewOptions{.follow_end = true, .primary = true});
    auto* transcript_pointer = transcript.get();
    auto focus = std::make_unique<FocusProbe>();
    auto* focus_pointer = focus.get();
    cch::tui::VStack root;
    REQUIRE(root.add_child(std::move(transcript), cch::tui::StackEntryOptions{.basis = 0, .grow = 1}));
    REQUIRE(root.add_child(std::move(focus), cch::tui::StackEntryOptions{.basis = 1, .shrink = 0}));
    tui.set_layout_root(std::make_unique<cch::tui::VStack>(std::move(root)));
    auto context = std::make_shared<cch::tui::KeybindingsContext>(fullscreen_registry());
    tui.set_keybinding_context(context);
    REQUIRE(tui.set_focus(focus_pointer));
    REQUIRE(tui.start());
    REQUIRE(tui.render());
    CHECK(transcript_pointer->scroll_top() == 9);

    REQUIRE(terminal.inject_input("\x1bOH"));
    REQUIRE(terminal.inject_input("\x1b[F"));
    REQUIRE(terminal.inject_input("\x1b[57423u"));
    REQUIRE(terminal.inject_input("\x1b[5;5~"));
    REQUIRE(terminal.inject_input("\x1b[6;5~"));
    CHECK(transcript_pointer->scroll_top() == 9);
    CHECK(focus_pointer->keys_received == 5);

    REQUIRE(terminal.inject_input("\x1b[1;5H"));
    REQUIRE(tui.render());
    CHECK(transcript_pointer->scroll_top() == 0);
    CHECK(focus_pointer->keys_received == 5);
    CHECK(terminal.screen()[2].find("Jump end") != std::string::npos);

    REQUIRE(terminal.inject_input("\x1b[57423;5:3u"));
    REQUIRE(tui.render());
    CHECK(transcript_pointer->scroll_top() == 0);
    CHECK(focus_pointer->keys_received == 5);

    REQUIRE(terminal.inject_input("\x1b[H"));
    CHECK(focus_pointer->keys_received == 6);

    REQUIRE(terminal.inject_input("\x1b[1;5F"));
    REQUIRE(tui.render());
    CHECK(transcript_pointer->scroll_top() == 9);
    CHECK(transcript_pointer->following_end());

    context->replace(fullscreen_registry({
            cch::tui::KeybindingOverride{.id = "tui.altScreen.top", .keys = {"ctrl+t"}},
            cch::tui::KeybindingOverride{.id = "tui.altScreen.bottom", .keys = {"ctrl+b"}},
    }));
    REQUIRE(terminal.inject_input("\x1b[1;5H"));
    REQUIRE(tui.render());
    CHECK(transcript_pointer->scroll_top() == 9);
    CHECK(focus_pointer->keys_received == 7);
    REQUIRE(terminal.inject_input("\x14"));
    REQUIRE(tui.render());
    CHECK(transcript_pointer->scroll_top() == 0);
    CHECK(terminal.screen()[2].find("Jump end") != std::string::npos);

    // The indicator press jumps before component dispatch and resumes follow-end.
    REQUIRE(terminal.inject_input("\x1b[<0;7;3M"));
    REQUIRE(tui.render());
    CHECK(transcript_pointer->scroll_top() == 9);
    CHECK(transcript_pointer->following_end());
    CHECK(focus_pointer->keys_received == 7);
    REQUIRE(tui.stop());
}

#include <cch/tui/AltScreen.hpp>
#include <cch/tui/HStack.hpp>
#include <cch/tui/Keybindings.hpp>
#include <cch/tui/MouseRegion.hpp>
#include <cch/tui/ScrollView.hpp>
#include <cch/tui/Text.hpp>
#include <cch/tui/VStack.hpp>
#include <cch/tui/VirtualTerminal.hpp>

#include "support/FullscreenKeybindings.hpp"

using cch::tests::fullscreen_registry;
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

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

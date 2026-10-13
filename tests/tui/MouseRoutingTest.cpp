#include <cch/tui/AltScreen.hpp>
#include <cch/tui/Container.hpp>
#include <cch/tui/Mouse.hpp>
#include <cch/tui/MouseRegion.hpp>
#include <cch/tui/Text.hpp>
#include <cch/tui/Tui.hpp>
#include <cch/tui/VStack.hpp>
#include <cch/tui/VirtualTerminal.hpp>

#include "tui/InputDecoder.hpp"
#include "tui/MouseRouting.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using cch::tui::MouseButton;
using cch::tui::MouseEvent;
using cch::tui::MouseEventResult;
using cch::tui::MouseEventType;
using cch::tui::MouseRegion;
using cch::tui::VirtualTerminal;

[[nodiscard]] constexpr std::string_view type_name(MouseEventType type) {
    switch (type) {
    case MouseEventType::Press:
        return "press";
    case MouseEventType::Release:
        return "release";
    case MouseEventType::Move:
        return "move";
    case MouseEventType::Drag:
        return "drag";
    case MouseEventType::Click:
        return "click";
    case MouseEventType::Wheel:
        return "wheel";
    }
    return "unknown";
}

/// A component that answers mouse events through a test-supplied policy, and
/// records every event it received.
class MouseProbe final : public cch::tui::Component,
                         public cch::tui::MouseHandler,
                         public cch::tui::Focusable,
                         public cch::tui::InputHandler {
public:
    using Handler = std::function<std::optional<MouseEventResult>(const MouseEvent&)>;

    explicit MouseProbe(std::vector<std::string> lines, Handler handler = {})
        : lines_(std::move(lines)), handler_(std::move(handler)) {}

    void set_handler(Handler handler) { handler_ = std::move(handler); }

    [[nodiscard]] cch::support::Expected<cch::tui::RenderResult> render(std::size_t) override {
        ++renders;
        return cch::tui::RenderResult{.lines = lines_};
    }

    void invalidate() override { ++invalidations; }

    [[nodiscard]] std::optional<cch::tui::CursorPosition> cursor_location() const override { return std::nullopt; }

    cch::tui::InputAdmissionOutcome handle_input(const cch::tui::InputEventVariant&) override {
        ++keys;
        return cch::tui::InputAdmissionOutcome::Consumed;
    }

    void set_focused(bool focused) override { focus_events.push_back(focused); }

    [[nodiscard]] bool focused() const override { return !focus_events.empty() && focus_events.back(); }

    [[nodiscard]] std::optional<MouseEventResult> handle_mouse(const MouseEvent& event) override {
        events.push_back(event);
        return handler_ ? handler_(event) : std::nullopt;
    }

    /// `type@localX,localY#screenX,screenY` per received event.
    [[nodiscard]] std::vector<std::string> labels() const {
        std::vector<std::string> result;
        result.reserve(events.size());
        for (const auto& event : events) {
            result.push_back(std::string{type_name(event.type)} + "@" + std::to_string(event.x) + "," +
                             std::to_string(event.y) + "#" + std::to_string(event.screen_x) + "," +
                             std::to_string(event.screen_y));
        }
        return result;
    }

    std::vector<MouseEvent> events;
    std::vector<bool> focus_events;
    std::size_t renders{0};
    std::size_t invalidations{0};
    std::size_t keys{0};

private:
    std::vector<std::string> lines_;
    Handler handler_;
};

[[nodiscard]] MouseEvent pointer_event(MouseEventType type, std::ptrdiff_t x, std::ptrdiff_t y) {
    return MouseEvent{.type = type,
            .button = MouseButton::Left,
            .x = x,
            .y = y,
            .screen_x = x,
            .screen_y = y,
            .width = 20,
            .height = 4};
}

[[nodiscard]] std::string all_output(const VirtualTerminal& terminal) {
    std::string output;
    for (const auto& chunk : terminal.output())
        output += chunk;
    return output;
}

/// A region handler that answers `answer` for the listed event types.
[[nodiscard]] cch::tui::MouseRegionHandler answer_types(std::vector<MouseEventType> types, MouseEventResult answer) {
    return [types = std::move(types), answer](const MouseEvent& event) -> std::optional<MouseEventResult> {
        for (const auto type : types) {
            if (type == event.type) return answer;
        }
        return std::nullopt;
    };
}

} // namespace

TEST_CASE("mouse reports parse from SGR, legacy X10, and wheel forms", "[tui][mouse][issue1002][spec]") {
    using cch::tui::detail::MousePacket;
    using cch::tui::detail::parse_mouse_packet;

    // SGR: one-based columns and rows, button code first.
    const auto press = parse_mouse_packet("\x1b[<12;3;2M");
    REQUIRE(press.has_value());
    CHECK(press->kind == MousePacket::Kind::Pointer);
    CHECK(press->button == 12);
    CHECK(press->x == 2);
    CHECK(press->y == 1);
    CHECK_FALSE(press->release);

    const auto release = parse_mouse_packet("\x1b[<0;3;2m");
    REQUIRE(release.has_value());
    CHECK(release->release);

    // Wheel: bit 6 with direction bits 0 (up) and 1 (down).
    const auto up = parse_mouse_packet("\x1b[<64;1;1M");
    REQUIRE(up.has_value());
    CHECK(up->kind == MousePacket::Kind::Wheel);
    CHECK(up->direction == -1);
    const auto down = parse_mouse_packet("\x1b[<65;1;1M");
    REQUIRE(down.has_value());
    CHECK(down->kind == MousePacket::Kind::Wheel);
    CHECK(down->direction == 1);
    // Direction 2 is a horizontal wheel report: pi rejects it as a wheel event
    // and the press parse keeps it.
    const auto horizontal = parse_mouse_packet("\x1b[<66;1;1M");
    REQUIRE(horizontal.has_value());
    CHECK(horizontal->kind == MousePacket::Kind::Pointer);

    // Legacy X10: six bytes, payload offset by 32 (button) and 33 (cells). Only
    // the wheel form is a report here; a legacy pointer frame is consumed as a
    // mouse sequence and yields no event (pi `parseWheelEvent`).
    const auto legacy_up = parse_mouse_packet("\x1b[M`!!");
    REQUIRE(legacy_up.has_value());
    CHECK(legacy_up->kind == MousePacket::Kind::Wheel);
    CHECK(legacy_up->button == 64);
    CHECK(legacy_up->direction == -1);
    CHECK(legacy_up->x == 0);
    CHECK(legacy_up->y == 0);
    const auto legacy_down = parse_mouse_packet("\x1b[Ma!!");
    REQUIRE(legacy_down.has_value());
    CHECK(legacy_down->kind == MousePacket::Kind::Wheel);
    CHECK(legacy_down->direction == 1);
    CHECK_FALSE(parse_mouse_packet("\x1b[M !!").has_value());
    CHECK_FALSE(parse_mouse_packet("\x1b[M#!!").has_value());

    CHECK_FALSE(parse_mouse_packet("\x1b[<0;3;2").has_value());
    CHECK_FALSE(parse_mouse_packet("\x1b[M ").has_value());
    CHECK_FALSE(parse_mouse_packet("x").has_value());

    CHECK(cch::tui::detail::is_mouse_sequence("\x1b[<0;3;2M"));
    CHECK(cch::tui::detail::is_mouse_sequence("\x1b[M !!"));
    CHECK_FALSE(cch::tui::detail::is_mouse_sequence("\x1b[A"));
    CHECK_FALSE(cch::tui::detail::is_mouse_sequence("x"));
}

TEST_CASE("a mouse report normalizes into a cell event", "[tui][mouse][issue1002][spec]") {
    using cch::tui::detail::create_mouse_event;
    using cch::tui::detail::parse_mouse_packet;
    using cch::tui::detail::pointer_event_type;

    const auto packet = parse_mouse_packet("\x1b[<20;4;3M");
    REQUIRE(packet.has_value());
    CHECK(pointer_event_type(*packet) == MouseEventType::Press);
    const auto event = create_mouse_event(*packet, pointer_event_type(*packet), 20, 4);
    CHECK(event.type == MouseEventType::Press);
    CHECK(event.button == MouseButton::Left);
    CHECK(event.x == 3);
    CHECK(event.y == 2);
    CHECK(event.screen_x == 3);
    CHECK(event.screen_y == 2);
    CHECK(event.width == 20);
    CHECK(event.height == 4);
    // Button 20 sets the shift (4) and control (16) modifier bits.
    CHECK(event.shift);
    CHECK(event.ctrl);
    CHECK_FALSE(event.alt);
    CHECK_FALSE(event.wheel_delta.has_value());
    CHECK_FALSE(event.click_count.has_value());

    // Motion without a button is a move, motion with one is a drag.
    const auto move_packet = parse_mouse_packet("\x1b[<35;1;1M");
    REQUIRE(move_packet.has_value());
    CHECK(pointer_event_type(*move_packet) == MouseEventType::Move);
    const auto drag_packet = parse_mouse_packet("\x1b[<32;1;1M");
    REQUIRE(drag_packet.has_value());
    CHECK(pointer_event_type(*drag_packet) == MouseEventType::Drag);
    const auto release_packet = parse_mouse_packet("\x1b[<0;1;1m");
    REQUIRE(release_packet.has_value());
    CHECK(pointer_event_type(*release_packet) == MouseEventType::Release);

    // A wheel event carries a delta, no button, and the optional click count.
    const auto wheel_packet = parse_mouse_packet("\x1b[<65;2;1M");
    REQUIRE(wheel_packet.has_value());
    const auto wheel =
            create_mouse_event(*wheel_packet, MouseEventType::Wheel, 20, 4, std::ptrdiff_t{1}, std::size_t{2});
    CHECK(wheel.button == MouseButton::None);
    CHECK(wheel.wheel_delta == std::optional<std::ptrdiff_t>{1});
    CHECK(wheel.click_count == std::optional<std::size_t>{2});

    // A terminal without measurable dimensions still reports one cell.
    const auto tiny_packet = parse_mouse_packet("\x1b[<0;1;1M");
    REQUIRE(tiny_packet.has_value());
    const auto tiny = create_mouse_event(*tiny_packet, MouseEventType::Press, 0, 0);
    CHECK(tiny.width == 1);
    CHECK(tiny.height == 1);

    // Move and release default to no render; press, click, drag and wheel do.
    CHECK_FALSE(cch::tui::mouse_event_renders_by_default(MouseEventType::Move));
    CHECK_FALSE(cch::tui::mouse_event_renders_by_default(MouseEventType::Release));
    CHECK(cch::tui::mouse_event_renders_by_default(MouseEventType::Press));
    CHECK(cch::tui::mouse_event_renders_by_default(MouseEventType::Click));
    CHECK(cch::tui::mouse_event_renders_by_default(MouseEventType::Drag));
    CHECK(cch::tui::mouse_event_renders_by_default(MouseEventType::Wheel));
}

TEST_CASE("dispatch resolves the target, focus, and coordinate transform", "[tui][mouse][issue1002][spec]") {
    // A handler that records without answering does not handle the event.
    MouseProbe silent({""});
    CHECK_FALSE(cch::tui::dispatch_mouse_event(silent, pointer_event(MouseEventType::Press, 1, 1)).has_value());
    CHECK(silent.events.size() == 1);

    // handled, capture, and focus stand alone: any one of them resolves the
    // event, and only focus names a focus target.
    const auto check_flag = [](MouseEventResult answer) {
        MouseProbe probe({""}, [answer](const MouseEvent&) { return std::optional<MouseEventResult>{answer}; });
        auto event = pointer_event(MouseEventType::Press, 4, 2);
        event.screen_x = 4;
        event.screen_y = 2;
        const auto result = cch::tui::dispatch_mouse_event(probe, event);
        REQUIRE(result.has_value());
        CHECK(result->handled);
        CHECK(result->target.component == &probe);
        CHECK(result->target.origin_x == 0);
        CHECK(result->target.origin_y == 0);
        CHECK(result->target.width == 20);
        CHECK(result->target.height == 4);
        CHECK(result->capture == answer.capture);
        CHECK(result->focus == answer.focus);
        CHECK(result->focus_target == (answer.focus ? &probe : nullptr));
    };
    check_flag(MouseEventResult{.handled = true});
    check_flag(MouseEventResult{.capture = true});
    check_flag(MouseEventResult{.focus = true});

    // Local coordinates name the receiving component's origin, so a captured
    // target keeps the transform it was hit at.
    MouseProbe probe(
            {""}, [](const MouseEvent&) { return std::optional<MouseEventResult>{MouseEventResult{.handled = true}}; });
    auto boxed = pointer_event(MouseEventType::Press, 2, 1);
    boxed.screen_x = 7;
    boxed.screen_y = 5;
    boxed.width = 4;
    boxed.height = 3;
    const auto result = cch::tui::dispatch_mouse_event(probe, boxed);
    REQUIRE(result.has_value());
    CHECK(result->target.origin_x == 5);
    CHECK(result->target.origin_y == 4);
    const auto retargeted = cch::tui::retarget_mouse_event(pointer_event(MouseEventType::Drag, 99, 99), result->target);
    CHECK(retargeted.x == 94);
    CHECK(retargeted.y == 95);
    CHECK(retargeted.width == 4);
    CHECK(retargeted.height == 3);

    // A forwarding host keeps the child's target; it only takes focus over when
    // it is a component that routes keys itself.
    auto child = std::make_unique<MouseProbe>(std::vector<std::string>{""},
            [](const MouseEvent&) { return std::optional<MouseEventResult>{MouseEventResult{.focus = true}}; });
    auto* child_pointer = child.get();
    MouseRegion region(std::move(child), {});
    const auto forwarded = cch::tui::dispatch_mouse_event(region, pointer_event(MouseEventType::Press, 1, 0));
    REQUIRE(forwarded.has_value());
    CHECK(forwarded->target.component == child_pointer);
    CHECK(forwarded->focus);
    CHECK(forwarded->focus_target == child_pointer);
}

TEST_CASE("MouseRegion delegates rendering and answers only what its child ignores", "[tui][mouse][issue1002][spec]") {
    auto child = std::make_unique<MouseProbe>(std::vector<std::string>{"child"},
            [](const MouseEvent&) { return std::optional<MouseEventResult>{MouseEventResult{.handled = true}}; });
    auto* child_pointer = child.get();
    std::size_t region_answers = 0;
    MouseRegion region(std::move(child), [&region_answers](const MouseEvent&) {
        ++region_answers;
        return std::optional<MouseEventResult>{MouseEventResult{.handled = true}};
    });

    REQUIRE(region.render(4).has_value());
    CHECK(child_pointer->renders == 1);
    region.invalidate();
    CHECK(child_pointer->invalidations == 1);
    CHECK(region.contains(child_pointer));
    CHECK(region.contains(&region));

    // The child handled it, so the region's own handler never runs.
    CHECK(region.handle_mouse_forwarded(pointer_event(MouseEventType::Press, 0, 0)).has_value());
    CHECK(region_answers == 0);

    // The child ignores it, so the region answers for itself.
    child_pointer->set_handler({});
    CHECK_FALSE(region.handle_mouse_forwarded(pointer_event(MouseEventType::Click, 1, 0)).has_value());
    CHECK(region.handle_mouse(pointer_event(MouseEventType::Click, 1, 0)).has_value());
    CHECK(region_answers == 1);
}

TEST_CASE("Container routes a cached-row hit to the child that occupied the row", "[tui][mouse][issue1002][spec]") {
    cch::tui::Container container;
    auto first = std::make_unique<MouseProbe>(std::vector<std::string>{"a", "b"},
            [](const MouseEvent&) { return std::optional<MouseEventResult>{MouseEventResult{.handled = true}}; });
    auto second = std::make_unique<MouseProbe>(std::vector<std::string>{"c"},
            [](const MouseEvent&) { return std::optional<MouseEventResult>{MouseEventResult{.handled = true}}; });
    auto* first_pointer = first.get();
    auto* second_pointer = second.get();
    REQUIRE(container.add_child(std::move(first)));
    REQUIRE(container.add_child(std::move(second)));

    // Before a composition there is no cached geometry to hit.
    CHECK_FALSE(container.handle_mouse_forwarded(pointer_event(MouseEventType::Press, 0, 0)).has_value());

    REQUIRE(container.render(10).has_value());
    const auto renders_after_composition = std::pair{first_pointer->renders, second_pointer->renders};

    auto event = pointer_event(MouseEventType::Press, 0, 2);
    event.height = 3;
    const auto hit = container.handle_mouse_forwarded(event);
    REQUIRE(hit.has_value());
    CHECK(hit->target.component == second_pointer);
    CHECK(hit->target.origin_y == 2);
    REQUIRE(second_pointer->events.size() == 1);
    CHECK(second_pointer->events.back().y == 0);
    CHECK(second_pointer->events.back().height == 1);
    CHECK(second_pointer->events.back().screen_y == 2);
    CHECK(first_pointer->events.empty());
    CHECK(std::pair{first_pointer->renders, second_pointer->renders} == renders_after_composition);

    // Row 1 belongs to the first child, and finding that out rendered nothing.
    auto middle = pointer_event(MouseEventType::Press, 0, 1);
    middle.height = 3;
    const auto second_hit = container.handle_mouse_forwarded(middle);
    REQUIRE(second_hit.has_value());
    CHECK(second_hit->target.component == first_pointer);
    CHECK(second_hit->target.origin_y == 0);
    CHECK(first_pointer->events.back().y == 1);
    CHECK(std::pair{first_pointer->renders, second_pointer->renders} == renders_after_composition);

    // Outside the composed rows is not this container's event.
    auto below = pointer_event(MouseEventType::Press, 0, 3);
    below.height = 3;
    CHECK_FALSE(container.handle_mouse_forwarded(below).has_value());
    CHECK_FALSE(container.handle_mouse_forwarded(pointer_event(MouseEventType::Press, 0, -1)).has_value());
}

TEST_CASE("Box offsets a hit by its padding and content width", "[tui][mouse][issue1002][spec]") {
    cch::tui::Box box(1, 1);
    auto child = std::make_unique<MouseProbe>(std::vector<std::string>{"one", "two"},
            [](const MouseEvent&) { return std::optional<MouseEventResult>{MouseEventResult{.handled = true}}; });
    auto* child_pointer = child.get();
    REQUIRE(box.add_child(std::move(child)));
    REQUIRE(box.render(8).has_value());

    // The first row and column belong to the box's padding.
    CHECK_FALSE(box.handle_mouse_forwarded(pointer_event(MouseEventType::Press, 1, 0)).has_value());
    CHECK_FALSE(box.handle_mouse_forwarded(pointer_event(MouseEventType::Press, 0, 1)).has_value());
    // The content is six columns wide starting at column 1.
    CHECK_FALSE(box.handle_mouse_forwarded(pointer_event(MouseEventType::Press, 7, 2)).has_value());

    auto event = pointer_event(MouseEventType::Press, 2, 2);
    event.width = 8;
    event.height = 4;
    const auto hit = box.handle_mouse_forwarded(event);
    REQUIRE(hit.has_value());
    CHECK(hit->target.component == child_pointer);
    CHECK(hit->target.origin_x == 1);
    CHECK(hit->target.origin_y == 1);
    REQUIRE(child_pointer->events.size() == 1);
    CHECK(child_pointer->events.back().x == 1);
    CHECK(child_pointer->events.back().y == 1);
    CHECK(child_pointer->events.back().width == 6);
    CHECK(child_pointer->events.back().height == 2);

    // The content's second row (frame row 2) is the child's second line.
    const auto second_line = box.handle_mouse_forwarded(pointer_event(MouseEventType::Press, 1, 2));
    REQUIRE(second_line.has_value());
    CHECK(child_pointer->events.back().y == 1);

    // Below the child's two rows the box routes nothing.
    CHECK_FALSE(box.handle_mouse_forwarded(pointer_event(MouseEventType::Press, 1, 4)).has_value());
}

TEST_CASE("AltScreen clicks a nested mouse region through the cached frame", "[tui][mouse][issue1002][spec]") {
    VirtualTerminal terminal({.columns = 20, .rows = 6});
    cch::tui::AltScreen tui(terminal);

    // The region answers presses (so the press becomes a gesture target) while
    // the probe underneath only records what it received.
    auto probe = std::make_unique<MouseProbe>(std::vector<std::string>{"one", "two"});
    auto* probe_pointer = probe.get();
    std::size_t region_clicks = 0;
    std::vector<MouseEvent> region_events;
    auto region = std::make_unique<MouseRegion>(
            std::move(probe), [&](const MouseEvent& event) -> std::optional<MouseEventResult> {
                region_events.push_back(event);
                if (event.type == MouseEventType::Press) return MouseEventResult{.handled = true, .render = false};
                if (event.type == MouseEventType::Click) {
                    ++region_clicks;
                    return MouseEventResult{.handled = true};
                }
                return std::nullopt;
            });
    auto box = std::make_unique<cch::tui::Box>(1, 1);
    REQUIRE(box->add_child(std::move(region)));
    auto stack = std::make_unique<cch::tui::VStack>();
    REQUIRE(stack->add_child(std::move(box)));
    tui.set_layout_root(std::move(stack));

    REQUIRE(tui.start());
    REQUIRE(tui.render());
    const auto renders_before = probe_pointer->renders;

    // Cell (2,2): inside the box padding by one cell, on the region's second row.
    REQUIRE(terminal.inject_input("\x1b[<0;3;3M"));
    REQUIRE(probe_pointer->events.size() == 1);
    CHECK(probe_pointer->labels() == std::vector<std::string>{"press@1,1#2,2"});
    REQUIRE(region_events.size() == 1);
    CHECK(region_events.back().screen_x == 2);
    CHECK(region_events.back().screen_y == 2);
    CHECK(region_events.back().width == 18);
    CHECK(region_events.back().height == 2);
    // The hit test read the cached frame: neither the probe nor the region
    // rendered to find the target.
    CHECK(probe_pointer->renders == renders_before);

    REQUIRE(terminal.inject_input("\x1b[<0;3;3m"));
    CHECK(region_clicks == 1);
    // The click reaches the same nested target with its own coordinates.
    REQUIRE(probe_pointer->labels().size() == 3);
    CHECK(probe_pointer->labels().back() == "click@1,1#2,2");
    CHECK(probe_pointer->events.back().click_count == std::optional<std::size_t>{1});
    CHECK(tui.focus() == nullptr);

    // A cell the box's padding owns is not a region event.
    REQUIRE(terminal.inject_input("\x1b[<0;1;1M"));
    REQUIRE(terminal.inject_input("\x1b[<0;1;1m"));
    CHECK(region_clicks == 1);
    CHECK(probe_pointer->events.size() == 3);
    CHECK(tui.stop());
}

TEST_CASE("AltScreen focuses and captures drag gestures for a component", "[tui][mouse][issue1002][spec]") {
    VirtualTerminal terminal({.columns = 20, .rows = 3});
    cch::tui::AltScreen tui(terminal);
    auto probe = std::make_unique<MouseProbe>(
            std::vector<std::string>{"control"}, [](const MouseEvent& event) -> std::optional<MouseEventResult> {
                if (event.type == MouseEventType::Press) {
                    return std::optional<MouseEventResult>{
                            MouseEventResult{.handled = true, .capture = true, .focus = true, .render = false}};
                }
                return std::optional<MouseEventResult>{MouseEventResult{.handled = true, .render = false}};
            });
    auto* probe_pointer = probe.get();
    REQUIRE(tui.add_child(std::move(probe)));
    REQUIRE(tui.start());
    REQUIRE(tui.render());

    REQUIRE(terminal.inject_input("\x1b[<0;1;1M"));
    CHECK(tui.focus() == probe_pointer);
    CHECK(probe_pointer->focused());

    // Drag and release outside the component's bounds still reach it, at its own
    // coordinates; the movement means the release is not a click.
    REQUIRE(terminal.inject_input("\x1b[<32;15;2M"));
    REQUIRE(terminal.inject_input("\x1b[<0;15;2m"));
    CHECK(probe_pointer->labels() == std::vector<std::string>{"press@0,0#0,0", "drag@14,1#14,1", "release@14,1#14,1"});

    // The gesture ended with the release: a later drag has no captured target.
    REQUIRE(terminal.inject_input("\x1b[<32;16;2M"));
    CHECK(probe_pointer->events.size() == 3);
    CHECK(tui.stop());
}

TEST_CASE("AltScreen reports consecutive click counts to a press target", "[tui][mouse][issue1002][spec]") {
    VirtualTerminal terminal({.columns = 20, .rows = 2});
    cch::tui::AltScreen tui(terminal);
    std::vector<std::size_t> click_counts;
    auto probe = std::make_unique<MouseProbe>(std::vector<std::string>{"control"},
            [&click_counts](const MouseEvent& event) -> std::optional<MouseEventResult> {
                if (event.type == MouseEventType::Press) {
                    return std::optional<MouseEventResult>{MouseEventResult{.handled = true, .render = false}};
                }
                if (event.type == MouseEventType::Click) {
                    click_counts.push_back(event.click_count.value_or(0));
                    return std::optional<MouseEventResult>{MouseEventResult{.handled = true, .render = false}};
                }
                return std::nullopt;
            });
    REQUIRE(tui.add_child(std::move(probe)));
    REQUIRE(tui.start());
    REQUIRE(tui.render());

    for (int count = 0; count < 3; ++count) {
        REQUIRE(terminal.inject_input("\x1b[<0;1;1M"));
        REQUIRE(terminal.inject_input("\x1b[<0;1;1m"));
    }
    CHECK(click_counts == std::vector<std::size_t>{1, 2, 3});

    // Another cell restarts the count.
    REQUIRE(terminal.inject_input("\x1b[<0;2;1M"));
    REQUIRE(terminal.inject_input("\x1b[<0;2;1m"));
    CHECK(click_counts == std::vector<std::size_t>{1, 2, 3, 1});
    CHECK(tui.stop());
}

TEST_CASE("AltScreen cancels a gesture on focus loss, removal, and stop", "[tui][mouse][issue1002][spec]") {
    VirtualTerminal terminal({.columns = 20, .rows = 3});
    cch::tui::AltScreen tui(terminal);
    auto probe = std::make_unique<MouseProbe>(
            std::vector<std::string>{"control"}, [](const MouseEvent& event) -> std::optional<MouseEventResult> {
                if (event.type == MouseEventType::Press) {
                    return std::optional<MouseEventResult>{
                            MouseEventResult{.handled = true, .capture = true, .focus = true, .render = false}};
                }
                return std::optional<MouseEventResult>{MouseEventResult{.handled = true, .render = false}};
            });
    auto* probe_pointer = probe.get();
    REQUIRE(tui.add_child(std::move(probe)));
    REQUIRE(tui.start());
    REQUIRE(tui.render());

    // Focus loss ends the gesture: the captured component hears nothing more.
    REQUIRE(terminal.inject_input("\x1b[<0;1;1M"));
    REQUIRE(probe_pointer->events.size() == 1);
    REQUIRE(terminal.inject_input("\x1b[O"));
    REQUIRE(terminal.inject_input("\x1b[<32;10;2M"));
    REQUIRE(terminal.inject_input("\x1b[<0;10;2m"));
    CHECK(probe_pointer->labels() == std::vector<std::string>{"press@0,0#0,0"});

    // Removing the component that owns the gesture cancels it too. The probe is
    // retired with the child operation that removed it, so the deliveries are
    // counted outside the component.
    auto delivered = std::make_shared<std::size_t>(0);
    auto removed = std::make_unique<MouseProbe>(std::vector<std::string>{"removed"},
            [delivered](const MouseEvent& event) -> std::optional<MouseEventResult> {
                ++*delivered;
                if (event.type == MouseEventType::Press) {
                    return std::optional<MouseEventResult>{
                            MouseEventResult{.handled = true, .capture = true, .render = false}};
                }
                return std::optional<MouseEventResult>{MouseEventResult{.handled = true, .render = false}};
            });
    auto* removed_pointer = removed.get();
    REQUIRE(tui.add_child(std::move(removed)));
    REQUIRE(tui.render());
    // The first probe holds row 0 of the document, so this one is on row 1.
    REQUIRE(terminal.inject_input("\x1b[<0;1;2M"));
    REQUIRE(*delivered == 1);
    REQUIRE(tui.remove_child(removed_pointer));
    REQUIRE(terminal.inject_input("\x1b[<32;10;2M"));
    REQUIRE(terminal.inject_input("\x1b[<0;10;2m"));
    CHECK(*delivered == 1);

    // A stopped root drops the gesture with the session.
    auto second = std::make_unique<MouseProbe>(
            std::vector<std::string>{"control"}, [](const MouseEvent& event) -> std::optional<MouseEventResult> {
                if (event.type == MouseEventType::Press) {
                    return std::optional<MouseEventResult>{
                            MouseEventResult{.handled = true, .capture = true, .render = false}};
                }
                return std::optional<MouseEventResult>{MouseEventResult{.handled = true, .render = false}};
            });
    auto* second_pointer = second.get();
    REQUIRE(tui.add_child(std::move(second)));
    REQUIRE(tui.start());
    REQUIRE(tui.render());
    // The first probe still holds row 0, so the press lands on row 1.
    REQUIRE(terminal.inject_input("\x1b[<0;1;2M"));
    REQUIRE(second_pointer->events.size() == 1);
    REQUIRE(tui.stop());
    REQUIRE(tui.start());
    // The report after the restart reaches the component under the pointer as a
    // plain release: it is not the captured gesture's release, so no click.
    REQUIRE(terminal.inject_input("\x1b[<0;1;2m"));
    CHECK(second_pointer->labels() == std::vector<std::string>{"press@0,0#0,1", "release@0,0#0,1"});
    CHECK(tui.stop());
}

TEST_CASE("a component removed from its container stops receiving the gesture", "[tui][mouse][issue1002][spec]") {
    VirtualTerminal terminal({.columns = 20, .rows = 3});
    cch::tui::AltScreen tui(terminal);

    // The container owns the probe, so removing the probe from it leaves the
    // object alive for this test to inspect.
    auto container = std::make_unique<cch::tui::Container>();
    auto* container_pointer = container.get();
    auto probe = std::make_unique<MouseProbe>(
            std::vector<std::string>{"control"}, [](const MouseEvent& event) -> std::optional<MouseEventResult> {
                if (event.type == MouseEventType::Press) {
                    return std::optional<MouseEventResult>{
                            MouseEventResult{.handled = true, .capture = true, .render = false}};
                }
                return std::optional<MouseEventResult>{MouseEventResult{.handled = true, .render = false}};
            });
    auto* probe_pointer = probe.get();
    REQUIRE(container->add_child(std::move(probe)));
    REQUIRE(tui.add_child(std::move(container)));
    REQUIRE(tui.start());
    REQUIRE(tui.render());

    REQUIRE(terminal.inject_input("\x1b[<0;1;1M"));
    REQUIRE(probe_pointer->events.size() == 1);

    // Detaching the captured component from its container ends the gesture: the
    // drag and release that follow are not delivered to it.
    const auto owned = container_pointer->remove_child(probe_pointer);
    REQUIRE(owned != nullptr);
    REQUIRE(terminal.inject_input("\x1b[<32;10;2M"));
    REQUIRE(terminal.inject_input("\x1b[<0;10;2m"));
    CHECK(probe_pointer->labels() == std::vector<std::string>{"press@0,0#0,0"});
    CHECK(tui.stop());
}

TEST_CASE("AltScreen consumes a wheel report a component handles", "[tui][mouse][issue1002][spec]") {
    VirtualTerminal terminal({.columns = 20, .rows = 3});
    cch::tui::AltScreen tui(terminal);
    std::vector<std::ptrdiff_t> deltas;
    auto region = std::make_unique<MouseRegion>(std::make_unique<cch::tui::Text>("1\n2\n3\n4\n5\n6\n7\n8", 0, 0),
            [&deltas](const MouseEvent& event) -> std::optional<MouseEventResult> {
                if (event.type != MouseEventType::Wheel) return std::nullopt;
                deltas.push_back(event.wheel_delta.value_or(0));
                return MouseEventResult{.handled = true, .render = false};
            });
    REQUIRE(tui.add_child(std::move(region)));
    REQUIRE(tui.start());
    REQUIRE(tui.render());
    const auto viewport_top = tui.viewport_top();

    REQUIRE(terminal.inject_input("\x1b[<64;1;1M"));
    CHECK(deltas == std::vector<std::ptrdiff_t>{-1});
    CHECK(tui.viewport_top() == viewport_top);
    CHECK(tui.stop());
}

TEST_CASE("AltScreen scrolls the viewport for an unconsumed wheel report", "[tui][mouse][issue1002][spec]") {
    VirtualTerminal terminal({.columns = 20, .rows = 3});
    cch::tui::AltScreen tui(terminal);
    auto probe = std::make_unique<MouseProbe>(std::vector<std::string>{"1", "2", "3", "4", "5", "6", "7", "8"});
    REQUIRE(tui.add_child(std::move(probe)));
    REQUIRE(tui.start());
    REQUIRE(tui.render());
    const auto viewport_top = tui.viewport_top();

    // The document follows its end, so scrolling up is the direction that moves.
    REQUIRE(terminal.inject_input("\x1b[<64;1;1M"));
    CHECK(tui.viewport_top() == viewport_top - 1);
    REQUIRE(terminal.inject_input("\x1b[<65;1;1M"));
    CHECK(tui.viewport_top() == viewport_top);
    CHECK(tui.stop());
}

TEST_CASE("AltScreen leaves handled pointer motion without a frame", "[tui][mouse][issue1002][spec]") {
    VirtualTerminal terminal({.columns = 20, .rows = 2});
    cch::tui::AltScreen tui(terminal);
    auto probe = std::make_unique<MouseProbe>(std::vector<std::string>{"hover target"},
            answer_types({MouseEventType::Move}, MouseEventResult{.handled = true}));
    auto* probe_pointer = probe.get();
    REQUIRE(tui.add_child(std::move(probe)));
    REQUIRE(tui.start());
    REQUIRE(tui.render());
    const auto renders_before = probe_pointer->renders;
    const auto writes_before = terminal.output().size();

    REQUIRE(terminal.inject_input("\x1b[<35;1;1M"));
    CHECK(probe_pointer->events.size() == 1);
    CHECK(probe_pointer->labels() == std::vector<std::string>{"move@0,0#0,0"});
    CHECK(probe_pointer->renders == renders_before);
    CHECK(terminal.output().size() == writes_before);
    CHECK(tui.stop());
}

TEST_CASE("AltScreen owns pointer reporting only while it runs", "[tui][mouse][issue1002][spec]") {
    VirtualTerminal terminal({.columns = 20, .rows = 2});
    cch::tui::AltScreen tui(terminal);
    auto probe = std::make_unique<MouseProbe>(std::vector<std::string>{"content"});
    auto* probe_pointer = probe.get();
    REQUIRE(tui.add_child(std::move(probe)));

    REQUIRE(tui.start());
    const auto started = all_output(terminal);
    // Enable all-motion tracking with SGR coordinates and focus reporting; a
    // multiplexer gets the button-motion form instead, which shares these four.
    CHECK(started.find("\x1b[?1000h") != std::string::npos);
    CHECK(started.find("\x1b[?1002h") != std::string::npos);
    CHECK(started.find("\x1b[?1004h") != std::string::npos);
    CHECK(started.find("\x1b[?1006h") != std::string::npos);

    // Focus reports are consumed by the root, either way.
    REQUIRE(terminal.inject_input("\x1b[I"));
    REQUIRE(terminal.inject_input("\x1b[O"));
    CHECK(probe_pointer->keys == 0);

    REQUIRE(tui.stop());
    const auto stopped = all_output(terminal);
    CHECK(stopped.find("\x1b[?1006l") != std::string::npos);
    CHECK(stopped.find("\x1b[?1004l") != std::string::npos);
    CHECK(stopped.find("\x1b[?1002l") != std::string::npos);
    CHECK(stopped.find("\x1b[?1000l") != std::string::npos);
}

TEST_CASE("the stream decoder reports a mouse report on its own channel", "[tui][mouse][issue1002][spec]") {
    cch::tui::detail::TerminalStreamDecoder decoder;
    const auto decoded = decoder.feed("\x1b[<0;2;1M");
    CHECK(decoded.mouse_frames == std::vector<std::string>{"\x1b[<0;2;1M"});
    CHECK(decoded.events.empty());
    // The report is not text: no payload byte may reach the character stream
    // (#949) and it decoded to no key event.
    CHECK(decoded.forwarded_input.empty());

    const auto legacy = decoder.feed("\x1b[M !!");
    CHECK(legacy.mouse_frames == std::vector<std::string>{"\x1b[M !!"});
    CHECK(legacy.events.empty());
    CHECK(legacy.forwarded_input.empty());

    // Typed input still travels the ordinary channel.
    const auto typed = decoder.feed("a");
    CHECK(typed.mouse_frames.empty());
    CHECK(typed.events.size() == 1);
    CHECK(typed.forwarded_input == "a");
}

TEST_CASE("regular mode neither consumes nor paints a mouse report", "[tui][mouse][issue1002][spec]") {
    VirtualTerminal terminal({.columns = 20, .rows = 3});
    cch::tui::MainScreen tui(terminal);
    auto probe = std::make_unique<MouseProbe>(std::vector<std::string>{"regular"});
    auto* probe_pointer = probe.get();
    REQUIRE(tui.add_child(std::move(probe)));
    REQUIRE(tui.start());
    REQUIRE(tui.render());

    REQUIRE(terminal.inject_input("\x1b[<0;2;1M"));
    CHECK(probe_pointer->keys == 0);
    CHECK(probe_pointer->events.empty());
    // Regular mode leaves the terminal's own mouse behavior alone.
    CHECK(all_output(terminal).find("\x1b[?1000h") == std::string::npos);
    CHECK(all_output(terminal).find("\x1b[?1006h") == std::string::npos);
    // Typed input after the report is unaffected.
    REQUIRE(terminal.inject_input("a"));
    CHECK(probe_pointer->keys == 1);
    CHECK(tui.stop());
}

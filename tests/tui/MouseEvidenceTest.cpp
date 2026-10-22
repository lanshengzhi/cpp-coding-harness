// Named pi-v1.0.4 acceptance for normalized mouse routing (#1002). Every
// expectation is read from the verified named bundle through
// `scripts/tui/evidence.py`; nothing here regenerates expectations from Pike or
// falls back to the historical `fixtures/pi-tui/*.json` root snapshots.

#include <cch/tui/AltScreen.hpp>
#include <cch/tui/Container.hpp>
#include <cch/tui/Input.hpp>
#include <cch/tui/Mouse.hpp>
#include <cch/tui/MouseRegion.hpp>
#include <cch/tui/Overlay.hpp>
#include <cch/tui/Text.hpp>
#include <cch/tui/Tui.hpp>
#include <cch/tui/VirtualTerminal.hpp>

#include "support/FullscreenKeybindings.hpp"
#include "support/PiTuiEvidence.hpp"
#include "support/RenderedScreen.hpp"
#include "tui/Layout.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using cch::support::JsonValue;
using cch::tui::MouseEvent;
using cch::tui::MouseEventResult;
using cch::tui::MouseEventType;

[[nodiscard]] std::ptrdiff_t int_of(const JsonValue& value) {
    return static_cast<std::ptrdiff_t>(std::llround(value.get_number()));
}

[[nodiscard]] std::vector<std::string> string_lines(const JsonValue& value) {
    std::vector<std::string> lines;
    for (const auto& line : value.get_array())
        lines.push_back(line.get_string());
    return lines;
}

/// The frame's painted rows with trailing padding removed: the frozen
/// observation uses pi's `visibleLines` convention (`test/layout.test.ts:10-12`).
[[nodiscard]] std::vector<std::string> drawn_lines(const cch::tui::detail::LayoutFrame& frame) {
    std::vector<std::string> rows;
    rows.reserve(frame.lines.size());
    for (const auto& row : frame.lines) {
        auto trimmed = row;
        while (!trimmed.empty() && trimmed.back() == ' ')
            trimmed.pop_back();
        rows.push_back(std::move(trimmed));
    }
    return rows;
}

[[nodiscard]] std::vector<std::string> trimmed_rows(const cch::tui::VirtualTerminal& terminal) {
    std::vector<std::string> rows;
    rows.reserve(terminal.screen().size());
    for (const auto& row : terminal.screen()) {
        auto trimmed = row;
        while (!trimmed.empty() && trimmed.back() == ' ')
            trimmed.pop_back();
        rows.push_back(std::move(trimmed));
    }
    return rows;
}

[[nodiscard]] cch::support::Expected<JsonValue> named_bundle() {
    return cch::tests::read_pi_tui_evidence("mouse.json");
}

[[nodiscard]] cch::support::Expected<JsonValue> overlay_routing_bundle() {
    return cch::tests::read_pi_v130_audit_evidence("audit-1050", "overlay-routing.json");
}

class OverlayWheelProbe final : public cch::tui::Component,
                                public cch::tui::Focusable,
                                public cch::tui::InputHandler,
                                public cch::tui::MouseHandler {
public:
    explicit OverlayWheelProbe(bool consume_wheel) : consume_wheel_(consume_wheel) {}

    [[nodiscard]] cch::support::Expected<cch::tui::RenderResult> render(std::size_t) override {
        return cch::tui::RenderResult{.lines = {"overlay"}};
    }

    void invalidate() override {}
    void set_focused(bool focused) override { focused_ = focused; }
    [[nodiscard]] bool focused() const override { return focused_; }

    cch::tui::InputAdmissionOutcome handle_input(const cch::tui::InputEventVariant& input) override {
        if (const auto* key = std::get_if<cch::tui::KeyEvent>(&input)) keys.push_back(key->key);
        return cch::tui::InputAdmissionOutcome::Consumed;
    }

    [[nodiscard]] std::optional<cch::tui::MouseEventResult> handle_mouse(const MouseEvent& event) override {
        if (event.type != MouseEventType::Wheel) return std::nullopt;
        ++wheel_events;
        if (consume_wheel_) return cch::tui::MouseEventResult{.handled = true};
        return std::nullopt;
    }

    std::size_t wheel_events{0};
    std::vector<std::string> keys;

private:
    bool consume_wheel_;
    bool focused_{false};
};

[[nodiscard]] const JsonValue& scenario_of(const JsonValue& bundle, std::string_view name) {
    for (const auto& scenario : bundle.at("scenarios").get_array()) {
        if (scenario.at("name").get_string() == name) return scenario;
    }
    REQUIRE(false);
    return bundle;
}

/// The `count`-row document of the frozen wheel observation (`line 1`, `line 2`,
/// ... as `mouse-scenarios.mts` builds it).
[[nodiscard]] std::string numbered_text(std::size_t count) {
    std::string text;
    for (std::size_t index = 1; index <= count; ++index) {
        if (index > 1) text += '\n';
        text += "line " + std::to_string(index);
    }
    return text;
}

/// One observed event as the frozen bundle records it.
struct ObservedEvent {
    std::string name;
    std::string type;
    std::ptrdiff_t x{0};
    std::ptrdiff_t y{0};
    std::ptrdiff_t screen_x{0};
    std::ptrdiff_t screen_y{0};
    std::size_t width{0};
    std::size_t height{0};
    bool shift{false};
    bool alt{false};
    bool ctrl{false};
    std::optional<std::size_t> click_count{};
};

[[nodiscard]] std::string type_name(MouseEventType type) {
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
    return "?";
}

/// A component that counts its renders and paints fixed rows, the C++ shape of
/// the frozen `{ render: () => [...] }` components.
class ProbeComponent final : public cch::tui::Component, public cch::tui::MouseHandler {
public:
    using Handler = std::function<std::optional<MouseEventResult>(const MouseEvent&)>;

    ProbeComponent(std::vector<std::string> lines, Handler handler, std::size_t* renders = nullptr)
        : lines_(std::move(lines)), handler_(std::move(handler)), renders_(renders) {}

    [[nodiscard]] cch::support::Expected<cch::tui::RenderResult> render(std::size_t) override {
        if (renders_ != nullptr) ++*renders_;
        return cch::tui::RenderResult{.lines = lines_};
    }

    void invalidate() override {}

    [[nodiscard]] std::optional<MouseEventResult> handle_mouse(const MouseEvent& event) override {
        return handler_(event);
    }

private:
    std::vector<std::string> lines_;
    Handler handler_;
    std::size_t* renders_;
};

/// A leaf that counts its renders and paints the rows of the text it stands for.
class CountingLeaf final : public cch::tui::Component {
public:
    CountingLeaf(std::string text, std::size_t& renders) : text_(std::move(text), 0, 0), renders_(&renders) {}

    [[nodiscard]] cch::support::Expected<cch::tui::RenderResult> render(std::size_t width) override {
        ++*renders_;
        return text_.render(width);
    }

    void invalidate() override { text_.invalidate(); }

    [[nodiscard]] std::optional<cch::tui::CursorPosition> cursor_location() const override {
        return text_.cursor_location();
    }

private:
    cch::tui::Text text_;
    std::size_t* renders_;
};

/// pi's `class FocusHost extends Container { handleInput() {} }`: a delegating
/// host that handles keyboard input, so a click on a nested control keeps focus
/// on the host (pi `tui.ts:81-89`).
class FocusHost final : public cch::tui::Component, public cch::tui::MouseHandler, public cch::tui::InputHandler {
public:
    explicit FocusHost(std::unique_ptr<cch::tui::Component> child) : child_(std::move(child)) {}

    [[nodiscard]] cch::support::Expected<cch::tui::RenderResult> render(std::size_t width) override {
        return child_->render(width);
    }

    void invalidate() override { child_->invalidate(); }

    [[nodiscard]] bool contains(const cch::tui::Component* target) const override {
        return this == target || child_->contains(target);
    }

    [[nodiscard]] std::optional<cch::tui::MouseDispatchResult> handle_mouse_forwarded(
            const MouseEvent& event) override {
        return cch::tui::dispatch_mouse_event(*child_, event);
    }

    cch::tui::InputAdmissionOutcome handle_input(const cch::tui::InputEventVariant&) override {
        return cch::tui::InputAdmissionOutcome::Unhandled;
    }

private:
    std::unique_ptr<cch::tui::Component> child_;
};

/// The frozen component name of a C++ component, for cross-side identity checks.
[[nodiscard]] std::string label_of(const std::vector<std::pair<const cch::tui::Component*, std::string>>& labels,
        const cch::tui::Component* component) {
    for (const auto& [candidate, name] : labels) {
        if (candidate == component) return name;
    }
    return "?";
}

void check_optional_render(const JsonValue& frozen, const std::optional<bool>& actual) {
    if (frozen.holds<JsonValue::null_t>()) {
        CHECK_FALSE(actual.has_value());
        return;
    }
    REQUIRE(actual.has_value());
    CHECK(actual.value() == frozen.get_boolean());
}

void check_optional_click_count(const JsonValue& frozen, const std::optional<std::size_t>& actual) {
    if (frozen.holds<JsonValue::null_t>()) {
        CHECK_FALSE(actual.has_value());
        return;
    }
    REQUIRE(actual.has_value());
    CHECK(actual.value() == static_cast<std::size_t>(int_of(frozen)));
}

} // namespace

TEST_CASE("the cached frame routes a press to the deepest nested region as frozen pi does",
        "[tui][mouse][issue1002][compat-pi]") {
    const auto bundle = named_bundle();
    REQUIRE(bundle);
    const auto& scenario = scenario_of(*bundle, "cached-frame-hit-test");
    const auto& frozen = scenario.at("expected");
    const auto& inputs = scenario.at("inputs");

    std::size_t alpha_renders = 0;
    std::size_t beta_renders = 0;
    std::vector<ObservedEvent> observed;
    const auto record = [&observed](std::string_view name, const MouseEvent& event) {
        observed.push_back(ObservedEvent{.name = std::string{name},
                .type = type_name(event.type),
                .x = event.x,
                .y = event.y,
                .screen_x = event.screen_x,
                .screen_y = event.screen_y,
                .width = event.width,
                .height = event.height,
                .shift = event.shift,
                .alt = event.alt,
                .ctrl = event.ctrl,
                .click_count = event.click_count});
    };
    const auto alpha_handler = [&record](const MouseEvent& event) -> std::optional<MouseEventResult> {
        record("alpha", event);
        if (event.type == MouseEventType::Press) {
            return std::optional<MouseEventResult>{MouseEventResult{.handled = true, .capture = true, .focus = true}};
        }
        return std::optional<MouseEventResult>{MouseEventResult{.handled = true}};
    };
    const auto beta_handler = [&record](const MouseEvent& event) -> std::optional<MouseEventResult> {
        record("beta", event);
        return std::nullopt;
    };

    auto alpha = std::make_unique<cch::tui::MouseRegion>(
            std::make_unique<CountingLeaf>("aa\nbb", alpha_renders), alpha_handler);
    auto beta =
            std::make_unique<cch::tui::MouseRegion>(std::make_unique<CountingLeaf>("cc", beta_renders), beta_handler);
    auto inner = std::make_unique<cch::tui::Container>();
    const auto* alpha_pointer = alpha.get();
    const auto* beta_pointer = beta.get();
    REQUIRE(inner->add_child(std::move(alpha)));
    REQUIRE(inner->add_child(std::move(beta)));
    const auto* inner_pointer = inner.get();
    cch::tui::Box box(1, 1);
    REQUIRE(box.add_child(std::move(inner)));
    const std::vector<std::pair<const cch::tui::Component*, std::string>> labels{
            {&box, "padded"}, {inner_pointer, "inner"}, {alpha_pointer, "alpha"}, {beta_pointer, "beta"}};
    CHECK(int_of(inputs.at("tree").at("box").at("paddingX")) == 1);
    CHECK(int_of(inputs.at("tree").at("box").at("paddingY")) == 1);

    const auto frame = cch::tui::detail::LayoutEngine::render(box, 10, 6);
    REQUIRE(frame);
    CHECK(drawn_lines(*frame) == string_lines(frozen.at("lines")));
    const auto renders_before = std::pair{alpha_renders, beta_renders};
    CHECK(alpha_renders == static_cast<std::size_t>(int_of(frozen.at("renderedBeforeProbes").at("alpha"))));
    CHECK(beta_renders == static_cast<std::size_t>(int_of(frozen.at("renderedBeforeProbes").at("beta"))));

    const auto& cells = frozen.at("cells").get_array();
    REQUIRE(cells.size() == inputs.at("probes").get_array().size());
    for (const auto& cell : cells) {
        const auto x = int_of(cell.at("x"));
        const auto y = int_of(cell.at("y"));
        const auto boxes = cch::tui::detail::layout_boxes_at(*frame, x, y);
        const auto& frozen_boxes = cell.at("boxes").get_array();
        REQUIRE(boxes.size() == frozen_boxes.size());
        for (std::size_t index = 0; index < boxes.size(); ++index) {
            const auto& frozen_box = frozen_boxes[index];
            CHECK(label_of(labels, boxes[index]->component) == frozen_box.at("component").get_string());
            CHECK(boxes[index]->rect.x == int_of(frozen_box.at("rect").at("x")));
            CHECK(boxes[index]->rect.y == int_of(frozen_box.at("rect").at("y")));
            CHECK(boxes[index]->rect.width == static_cast<std::size_t>(int_of(frozen_box.at("rect").at("width"))));
            CHECK(boxes[index]->rect.height == static_cast<std::size_t>(int_of(frozen_box.at("rect").at("height"))));
        }

        // pi `dispatchMouseToLayout`: offer the cell to each box deepest first,
        // in the box's own coordinates, and keep the first resolved answer.
        const MouseEvent event{.type = MouseEventType::Press,
                .button = cch::tui::MouseButton::Left,
                .x = x,
                .y = y,
                .screen_x = x,
                .screen_y = y,
                .width = 10,
                .height = 6};
        std::optional<cch::tui::MouseDispatchResult> result;
        std::vector<std::string> dispatched_to;
        std::vector<const cch::tui::Component*> visited;
        for (const auto* candidate : boxes) {
            if (candidate->component == nullptr) continue;
            if (std::find(visited.begin(), visited.end(), candidate->component) != visited.end()) continue;
            visited.push_back(candidate->component);
            dispatched_to.push_back(label_of(labels, candidate->component));
            auto target_event = event;
            target_event.x = x - candidate->rect.x;
            target_event.y = y - candidate->rect.y;
            target_event.width = candidate->rect.width;
            target_event.height = candidate->rect.height;
            result = cch::tui::dispatch_mouse_event(*candidate->component, target_event);
            if (result) break;
        }
        CHECK(dispatched_to == string_lines(cell.at("dispatchedTo")));
        const auto& frozen_result = cell.at("result");
        if (frozen_result.holds<JsonValue::null_t>()) {
            CHECK_FALSE(result.has_value());
            continue;
        }
        REQUIRE(result.has_value());
        CHECK(result->handled);
        CHECK(result->capture == frozen_result.at("capture").get_boolean());
        CHECK(result->focus == frozen_result.at("focus").get_boolean());
        check_optional_render(frozen_result.at("render"), result->render);
        CHECK(label_of(labels, result->target.component) == frozen_result.at("target").get_string());
        const auto& frozen_focus_target = frozen_result.at("focusTarget");
        if (frozen_focus_target.holds<JsonValue::null_t>()) {
            CHECK(result->focus_target == nullptr);
        } else {
            REQUIRE(result->focus_target != nullptr);
            CHECK(label_of(labels, result->focus_target) == frozen_focus_target.get_string());
        }
        CHECK(result->target.origin_x == int_of(frozen_result.at("local").at("x")));
        CHECK(result->target.origin_y == int_of(frozen_result.at("local").at("y")));
        CHECK(result->target.width == static_cast<std::size_t>(int_of(frozen_result.at("size").at("width"))));
        CHECK(result->target.height == static_cast<std::size_t>(int_of(frozen_result.at("size").at("height"))));
    }

    // The hit test read the cached composition rows: no probe rendered again.
    CHECK(std::pair{alpha_renders, beta_renders} == renders_before);
    CHECK(alpha_renders == static_cast<std::size_t>(int_of(frozen.at("renderedAfterProbes").at("alpha"))));
    CHECK(beta_renders == static_cast<std::size_t>(int_of(frozen.at("renderedAfterProbes").at("beta"))));

    const auto& frozen_observations = frozen.at("observations").get_array();
    REQUIRE(observed.size() == frozen_observations.size());
    for (std::size_t index = 0; index < observed.size(); ++index) {
        const auto& expected = frozen_observations[index];
        const auto& actual = observed[index];
        CHECK(actual.name == expected.at("region").get_string());
        CHECK(actual.type == expected.at("type").get_string());
        CHECK(actual.x == int_of(expected.at("x")));
        CHECK(actual.y == int_of(expected.at("y")));
        CHECK(actual.screen_x == int_of(expected.at("screenX")));
        CHECK(actual.screen_y == int_of(expected.at("screenY")));
        CHECK(actual.width == static_cast<std::size_t>(int_of(expected.at("width"))));
        CHECK(actual.height == static_cast<std::size_t>(int_of(expected.at("height"))));
        CHECK(actual.shift == expected.at("shift").get_boolean());
        CHECK(actual.alt == expected.at("alt").get_boolean());
        CHECK(actual.ctrl == expected.at("ctrl").get_boolean());
        CHECK_FALSE(actual.click_count.has_value());
    }

    // Retargeting a later report recreates the target's local coordinates.
    const auto& frozen_retarget = frozen.at("retarget");
    const auto& retarget_inputs = inputs.at("retarget");
    const auto retarget_boxes = cch::tui::detail::layout_boxes_at(
            *frame, int_of(retarget_inputs.at("cell").at("x")), int_of(retarget_inputs.at("cell").at("y")));
    REQUIRE_FALSE(retarget_boxes.empty());
    const auto* retarget_box = retarget_boxes.front();
    const MouseEvent drag{.type = MouseEventType::Drag,
            .button = cch::tui::MouseButton::Left,
            .x = int_of(retarget_inputs.at("event").at("x")),
            .y = int_of(retarget_inputs.at("event").at("y")),
            .screen_x = int_of(retarget_inputs.at("event").at("x")),
            .screen_y = int_of(retarget_inputs.at("event").at("y")),
            .width = 10,
            .height = 6};
    const cch::tui::MouseDispatchTarget target{.component = retarget_box->component,
            .origin_x = retarget_box->rect.x,
            .origin_y = retarget_box->rect.y,
            .width = retarget_box->rect.width,
            .height = retarget_box->rect.height};
    CHECK(label_of(labels, target.component) == frozen_retarget.at("target").get_string());
    const auto retargeted = cch::tui::retarget_mouse_event(drag, target);
    CHECK(retargeted.x == int_of(frozen_retarget.at("local").at("x")));
    CHECK(retargeted.y == int_of(frozen_retarget.at("local").at("y")));
    CHECK(retargeted.width == static_cast<std::size_t>(int_of(frozen_retarget.at("size").at("width"))));
    CHECK(retargeted.height == static_cast<std::size_t>(int_of(frozen_retarget.at("size").at("height"))));
}

TEST_CASE("a captured gesture follows the pointer outside its component and keeps host focus",
        "[tui][mouse][issue1002][compat-pi]") {
    const auto bundle = named_bundle();
    REQUIRE(bundle);
    const auto& scenario = scenario_of(*bundle, "capture-drag-and-host-focus");
    const auto& frozen = scenario.at("expected");
    const auto& inputs = scenario.at("inputs");

    std::vector<ObservedEvent> gesture;
    cch::tui::VirtualTerminal terminal({.columns = 20, .rows = 2});
    cch::tui::AltScreen tui(terminal);
    auto control = std::make_unique<ProbeComponent>(std::vector<std::string>{"control"},
            [&gesture](const MouseEvent& event) -> std::optional<MouseEventResult> {
                gesture.push_back(ObservedEvent{.name = "control",
                        .type = type_name(event.type),
                        .x = event.x,
                        .y = event.y,
                        .screen_x = event.screen_x,
                        .screen_y = event.screen_y,
                        .width = event.width,
                        .height = event.height});
                if (event.type == MouseEventType::Press) {
                    return std::optional<MouseEventResult>{
                            MouseEventResult{.handled = true, .capture = true, .focus = true}};
                }
                return std::optional<MouseEventResult>{MouseEventResult{.handled = true}};
            });
    auto* control_pointer = control.get();
    REQUIRE(tui.add_child(std::move(control)));
    REQUIRE(tui.start());
    REQUIRE(tui.render());
    for (const auto& report : inputs.at("reports").get_array())
        REQUIRE(terminal.inject_input(report.get_string()));

    const auto& frozen_events = frozen.at("gesture").at("events").get_array();
    REQUIRE(gesture.size() == frozen_events.size());
    for (std::size_t index = 0; index < gesture.size(); ++index) {
        const auto& expected = frozen_events[index];
        CHECK(gesture[index].type == expected.at("type").get_string());
        CHECK(gesture[index].x == int_of(expected.at("x")));
        CHECK(gesture[index].y == int_of(expected.at("y")));
        CHECK(gesture[index].screen_x == int_of(expected.at("screenX")));
        CHECK(gesture[index].screen_y == int_of(expected.at("screenY")));
        CHECK(gesture[index].width == static_cast<std::size_t>(int_of(expected.at("width"))));
        CHECK(gesture[index].height == static_cast<std::size_t>(int_of(expected.at("height"))));
    }
    REQUIRE(tui.focus() == control_pointer);
    CHECK(frozen.at("gesture").at("focused").get_string() == "control");
    CHECK(trimmed_rows(terminal) == string_lines(frozen.at("gesture").at("viewport")));
    REQUIRE(tui.stop());

    cch::tui::VirtualTerminal host_terminal({.columns = 20, .rows = 2});
    cch::tui::AltScreen host_tui(host_terminal);
    auto host = std::make_unique<FocusHost>(std::make_unique<cch::tui::MouseRegion>(
            std::make_unique<cch::tui::Text>("nested", 0, 0), [](const MouseEvent&) -> std::optional<MouseEventResult> {
                return std::optional<MouseEventResult>{MouseEventResult{.focus = true}};
            }));
    auto* host_pointer = host.get();
    REQUIRE(host_tui.add_child(std::move(host)));
    REQUIRE(host_tui.start());
    REQUIRE(host_tui.render());
    for (const auto& report : inputs.at("host").at("reports").get_array())
        REQUIRE(host_terminal.inject_input(report.get_string()));
    REQUIRE(host_tui.focus() == host_pointer);
    CHECK(frozen.at("hostFocus").at("focused").get_string() == "host");
    CHECK(inputs.at("host").at("hostHandlesInput").get_boolean());
    REQUIRE(host_tui.stop());
}

TEST_CASE("focused overlay wheel routing matches the frozen pi-v1.3.0 observation",
        "[tui][mouse][overlay][issue1125][compat-pi]") {
    const auto bundle = overlay_routing_bundle();
    REQUIRE(bundle);
    const auto& scenario = scenario_of(*bundle, "overlay-unhandled-wheel-scrolls-transcript");
    const auto& frozen = scenario.at("expected");
    const auto& viewport = frozen.at("viewportTop");

    cch::tui::VirtualTerminal terminal({.columns = 20, .rows = 6});
    cch::tui::AltScreen tui(terminal);
    REQUIRE(tui.add_child(std::make_unique<cch::tui::Text>(numbered_text(12), 0, 0)));
    tui.set_keybinding_context(std::make_shared<cch::tui::KeybindingsContext>(cch::tests::fullscreen_registry()));
    REQUIRE(tui.start());
    REQUIRE(tui.render());
    CHECK(tui.viewport_top() == static_cast<std::size_t>(int_of(viewport.at("before"))));

    auto overlay = std::make_unique<cch::tui::Overlay>();
    auto* overlay_pointer = overlay.get();
    overlay_pointer->set_options(cch::tui::OverlayOptions{
            .width = cch::tui::SizeValue::absolute(10), .anchor = cch::tui::OverlayAnchor::TopLeft});
    auto probe = std::make_unique<OverlayWheelProbe>(false);
    auto* probe_pointer = probe.get();
    REQUIRE(overlay_pointer->add_child(std::move(probe)));
    REQUIRE(overlay_pointer->focus_first());
    auto shown = tui.show_overlay(std::move(overlay));
    REQUIRE(shown);
    auto overlay_handle = std::move(*shown);
    REQUIRE(tui.render());
    CHECK(overlay_handle.is_focused() == frozen.at("overlayFocused").get_boolean());
    CHECK(tui.viewport_top() == static_cast<std::size_t>(int_of(viewport.at("afterOverlay"))));

    REQUIRE(terminal.inject_input("\x1b[<64;3;1M"));
    REQUIRE(tui.render());
    CHECK(probe_pointer->wheel_events == 1);
    CHECK(tui.viewport_top() == static_cast<std::size_t>(int_of(viewport.at("afterWheel"))));
    auto expected_screen = string_lines(frozen.at("screenAfterWheel"));
    for (auto& row : expected_screen)
        row = cch::tests::strip_ansi(row);
    CHECK(terminal.screen() == expected_screen);

    REQUIRE(terminal.inject_input("\x1b[6~"));
    REQUIRE(tui.render());
    CHECK(probe_pointer->keys == std::vector<std::string>{"pageDown"});
    CHECK(tui.viewport_top() == static_cast<std::size_t>(int_of(viewport.at("afterPageDown"))));
    CHECK(tui.stop());

    cch::tui::VirtualTerminal consumed_terminal({.columns = 20, .rows = 6});
    cch::tui::AltScreen consumed_tui(consumed_terminal);
    REQUIRE(consumed_tui.add_child(std::make_unique<cch::tui::Text>(numbered_text(12), 0, 0)));
    REQUIRE(consumed_tui.start());
    REQUIRE(consumed_tui.render());
    const auto consumed_top = consumed_tui.viewport_top();
    auto consumed_overlay = std::make_unique<cch::tui::Overlay>(cch::tui::OverlayOptions{
            .width = cch::tui::SizeValue::absolute(10), .anchor = cch::tui::OverlayAnchor::TopLeft});
    auto consumed_probe = std::make_unique<OverlayWheelProbe>(true);
    auto* consumed_probe_pointer = consumed_probe.get();
    REQUIRE(consumed_overlay->add_child(std::move(consumed_probe)));
    REQUIRE(consumed_overlay->focus_first());
    auto consumed_handle = consumed_tui.show_overlay(std::move(consumed_overlay));
    REQUIRE(consumed_handle);
    REQUIRE(consumed_tui.render());
    REQUIRE(consumed_handle->get_bounds());
    CHECK(consumed_handle->get_bounds()->row == 0);
    CHECK(consumed_handle->get_bounds()->col == 0);
    CHECK(consumed_handle->get_bounds()->width == 10);
    REQUIRE(consumed_terminal.inject_input("\x1b[<64;3;1M"));
    REQUIRE(consumed_tui.render());
    CHECK(consumed_probe_pointer->wheel_events == 1);
    CHECK(consumed_tui.viewport_top() == consumed_top);
    CHECK(consumed_tui.stop());
}

TEST_CASE("a consumed wheel report keeps the viewport and an unconsumed one scrolls it",
        "[tui][mouse][issue1002][compat-pi]") {
    const auto bundle = named_bundle();
    REQUIRE(bundle);
    const auto& scenario = scenario_of(*bundle, "wheel-consumed-and-unconsumed");
    const auto& frozen = scenario.at("expected");
    const auto& inputs = scenario.at("inputs");

    std::size_t wheel_events = 0;
    std::optional<std::ptrdiff_t> wheel_delta;
    cch::tui::VirtualTerminal terminal({.columns = 20, .rows = 3});
    cch::tui::AltScreen tui(terminal);
    REQUIRE(tui.add_child(std::make_unique<cch::tui::MouseRegion>(
            std::make_unique<cch::tui::Text>(
                    numbered_text(static_cast<std::size_t>(int_of(inputs.at("documentLines")))), 0, 0),
            [&wheel_events, &wheel_delta](const MouseEvent& event) -> std::optional<MouseEventResult> {
                if (event.type != MouseEventType::Wheel) return std::nullopt;
                ++wheel_events;
                wheel_delta = event.wheel_delta;
                return std::optional<MouseEventResult>{MouseEventResult{.handled = true}};
            })));
    REQUIRE(tui.start());
    REQUIRE(tui.render());
    CHECK(tui.viewport_top() == static_cast<std::size_t>(int_of(frozen.at("consumedTop"))));
    REQUIRE(terminal.inject_input(inputs.at("consumedReport").get_string()));
    REQUIRE(tui.render());
    const auto& consumed = frozen.at("consumedReport");
    CHECK(wheel_events == static_cast<std::size_t>(int_of(consumed.at("wheelEvents"))));
    REQUIRE(wheel_delta.has_value());
    CHECK(wheel_delta.value() == int_of(consumed.at("wheelDelta")));
    CHECK(tui.viewport_top() == static_cast<std::size_t>(int_of(consumed.at("viewportTop"))));
    CHECK(trimmed_rows(terminal) == string_lines(consumed.at("viewport")));
    REQUIRE(tui.stop());

    cch::tui::VirtualTerminal open_terminal({.columns = 20, .rows = 3});
    cch::tui::AltScreen open_tui(open_terminal);
    REQUIRE(open_tui.add_child(std::make_unique<cch::tui::Text>(
            numbered_text(static_cast<std::size_t>(int_of(inputs.at("documentLines")))), 0, 0)));
    REQUIRE(open_tui.start());
    REQUIRE(open_tui.render());
    const auto& phases = frozen.at("phases").get_array();
    REQUIRE(phases.size() == inputs.at("unconsumedReports").get_array().size() + 1);
    CHECK(phases.front().at("action").get_string() == "initial");
    CHECK(open_tui.viewport_top() == static_cast<std::size_t>(int_of(phases.front().at("viewportTop"))));
    CHECK(trimmed_rows(open_terminal) == string_lines(phases.front().at("viewport")));
    for (std::size_t index = 0; index < inputs.at("unconsumedReports").get_array().size(); ++index) {
        const auto& phase = phases[index + 1];
        REQUIRE(open_terminal.inject_input(inputs.at("unconsumedReports").get_array()[index].get_string()));
        REQUIRE(open_tui.render());
        CHECK(open_tui.viewport_top() == static_cast<std::size_t>(int_of(phase.at("viewportTop"))));
        CHECK(trimmed_rows(open_terminal) == string_lines(phase.at("viewport")));
    }
    REQUIRE(open_tui.stop());
}

TEST_CASE("handled motion writes nothing and consecutive clicks count up as frozen pi does",
        "[tui][mouse][issue1002][compat-pi]") {
    const auto bundle = named_bundle();
    REQUIRE(bundle);
    const auto& scenario = scenario_of(*bundle, "motion-and-click-counts");
    const auto& frozen = scenario.at("expected");
    const auto& inputs = scenario.at("inputs");

    std::size_t renders = 0;
    std::vector<ObservedEvent> events;
    cch::tui::VirtualTerminal terminal({.columns = 20, .rows = 2});
    cch::tui::AltScreen tui(terminal);
    REQUIRE(tui.add_child(std::make_unique<ProbeComponent>(
            std::vector<std::string>{"hover target"},
            [&events](const MouseEvent& event) -> std::optional<MouseEventResult> {
                if (event.type == MouseEventType::Move || event.type == MouseEventType::Press ||
                        event.type == MouseEventType::Click) {
                    events.push_back(ObservedEvent{.name = "control",
                            .type = type_name(event.type),
                            .x = event.x,
                            .y = event.y,
                            .click_count = event.click_count});
                }
                if (event.type == MouseEventType::Move || event.type == MouseEventType::Press ||
                        event.type == MouseEventType::Click) {
                    return std::optional<MouseEventResult>{MouseEventResult{.handled = true}};
                }
                return std::nullopt;
            },
            &renders)));
    REQUIRE(tui.start());
    REQUIRE(tui.render());

    const auto& motion = frozen.at("motion");
    CHECK(renders == static_cast<std::size_t>(int_of(motion.at("rendered"))));
    const auto writes_before = terminal.output().size();
    const auto renders_before = renders;
    REQUIRE(terminal.inject_input(inputs.at("motionReport").get_string()));
    CHECK(renders == renders_before);
    CHECK(terminal.output().size() == writes_before);
    // The frozen observation records the same invariance.
    CHECK(int_of(frozen.at("afterMotion").at("rendered")) == int_of(motion.at("rendered")));
    CHECK(int_of(frozen.at("afterMotion").at("writes")) == int_of(motion.at("writes")));

    for (std::ptrdiff_t repeat = 0; repeat < int_of(inputs.at("repeat")); ++repeat) {
        for (const auto& report : inputs.at("clickReports").get_array())
            REQUIRE(terminal.inject_input(report.get_string()));
    }
    const auto click_counts = [&events] {
        std::vector<std::size_t> counts;
        for (const auto& event : events) {
            if (event.type == "click" && event.click_count.has_value()) counts.push_back(event.click_count.value());
        }
        return counts;
    };
    const auto frozen_counts = [](const JsonValue& value) {
        std::vector<std::size_t> counts;
        for (const auto& count : value.get_array())
            counts.push_back(static_cast<std::size_t>(int_of(count)));
        return counts;
    };
    CHECK(click_counts() == frozen_counts(frozen.at("counts")));
    for (const auto& report : inputs.at("moveBetweenReports").get_array())
        REQUIRE(terminal.inject_input(report.get_string()));
    // A report between two presses restarts the count at one.
    CHECK(click_counts() == frozen_counts(frozen.at("afterMove")));

    const auto& frozen_events = frozen.at("events").get_array();
    REQUIRE(events.size() == frozen_events.size());
    for (std::size_t index = 0; index < events.size(); ++index) {
        const auto& expected = frozen_events[index];
        CHECK(events[index].type == expected.at("type").get_string());
        CHECK(events[index].x == int_of(expected.at("x")));
        CHECK(events[index].y == int_of(expected.at("y")));
        if (expected.at("type").get_string() == "click")
            check_optional_click_count(expected.at("clickCount"), events[index].click_count);
    }
    REQUIRE(tui.stop());
}

TEST_CASE("the regular screen leaves terminal-owned mouse behavior and typed input intact",
        "[tui][mouse][issue1002][compat-pi]") {
    const auto bundle = named_bundle();
    REQUIRE(bundle);
    const auto& scenario = scenario_of(*bundle, "regular-screen-keeps-terminal-mouse");
    const auto& frozen = scenario.at("expected").at("regular");
    const auto& inputs = scenario.at("inputs");

    cch::tui::VirtualTerminal terminal({.columns = 20, .rows = 3});
    cch::tui::MainScreen tui(terminal);
    auto input = std::make_unique<cch::tui::Input>();
    auto* input_pointer = input.get();
    REQUIRE(tui.add_child(std::move(input)));
    REQUIRE(tui.set_focus(input_pointer));
    REQUIRE(tui.start());
    REQUIRE(terminal.inject_input(inputs.at("reports").get_string()));
    REQUIRE(terminal.inject_input(inputs.at("typed").get_string()));
    REQUIRE(tui.render());

    CHECK(int_of(frozen.at("enabledMouse")) == 0);
    for (const auto& written : terminal.output()) {
        CHECK(written.find("\x1b[?100") == std::string::npos);
    }
    CHECK(input_pointer->value() == frozen.at("value").get_string());
    CHECK_FALSE(frozen.at("containsReport").get_boolean());
    CHECK(input_pointer->value().find("\x1b[<") == std::string::npos);
    REQUIRE(tui.stop());
}

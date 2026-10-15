// Named pi-v1.0.4 acceptance for scroll viewports (#1000). Every expectation is
// read from the verified named bundle through `scripts/tui/evidence.py`; nothing
// here regenerates expectations from Pike or falls back to the historical
// `fixtures/pi-tui/*.json` root snapshots.

#include <cch/tui/AltScreen.hpp>
#include <cch/tui/HStack.hpp>
#include <cch/tui/Keybindings.hpp>
#include <cch/tui/MouseRegion.hpp>
#include <cch/tui/ScrollView.hpp>
#include <cch/tui/Stack.hpp>
#include <cch/tui/Text.hpp>
#include <cch/tui/VStack.hpp>
#include <cch/tui/VirtualTerminal.hpp>

#include "support/FullscreenKeybindings.hpp"

using cch::tests::fullscreen_registry;
#include "support/PiTuiEvidence.hpp"
#include "tui/Layout.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <chrono>
#include <optional>

namespace {

using cch::support::JsonValue;

/// A leaf with one image placement, the C++ shape of pi's plain
/// `{ render: () => [...] }` transcript whose rows reserve an image line.
class ImageChild final : public cch::tui::Component {
public:
    ImageChild(std::vector<std::string> lines, cch::tui::InlineImageRenderRegion image)
        : lines_(std::move(lines)), image_(std::move(image)) {}

    [[nodiscard]] cch::support::Expected<cch::tui::RenderResult> render(std::size_t) override {
        return cch::tui::RenderResult{.lines = lines_, .images = {image_}};
    }

    void invalidate() override {}

private:
    std::vector<std::string> lines_;
    cch::tui::InlineImageRenderRegion image_;
};

/// The frame's painted rows with the leaf's width padding removed: the frozen
/// observation uses pi's `visibleLines` convention (strip terminal sequences and
/// trailing padding, `test/layout.test.ts:10-12`).
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

[[nodiscard]] std::ptrdiff_t int_of(const JsonValue& value) {
    return static_cast<std::ptrdiff_t>(std::llround(value.get_number()));
}

[[nodiscard]] std::vector<std::string> string_lines(const JsonValue& value) {
    std::vector<std::string> lines;
    for (const auto& line : value.get_array())
        lines.push_back(line.get_string());
    return lines;
}

/// A placement over `columns` cells and `rows` reserved cell rows at content
/// row `row`, the C++ shape of pi's registered kitty metadata.
[[nodiscard]] cch::tui::InlineImageRenderRegion placement(std::size_t row, std::size_t columns, std::size_t rows) {
    cch::tui::InlineImageRenderRegion image;
    image.resource_id = 124;
    image.region = cch::tui::CellRegion{.column = 0, .row = row, .columns = columns, .rows = rows};
    image.fallback_text = "im";
    image.rows_reserved_in_lines = true;
    return image;
}

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

/// The `count`-row transcript of the frozen observation (pi's `Text(numbered, 0, 0)`).
[[nodiscard]] std::string numbered_text(std::size_t count) {
    std::string text;
    for (std::size_t index = 1; index <= count; ++index) {
        if (index > 1) text += '\n';
        text += std::to_string(index);
    }
    return text;
}

class InputProbe final : public cch::tui::Component, public cch::tui::Focusable, public cch::tui::InputHandler {
public:
    [[nodiscard]] cch::support::Expected<cch::tui::RenderResult> render(std::size_t) override {
        return cch::tui::RenderResult{.lines = {"editor"}};
    }
    void invalidate() override {}
    cch::tui::InputAdmissionOutcome handle_input(const cch::tui::InputEventVariant& event) override {
        if (const auto* key = std::get_if<cch::tui::KeyEvent>(&event)) inputs.push_back(key->raw_sequence);
        return cch::tui::InputAdmissionOutcome::Consumed;
    }
    void set_focused(bool focused) override { focused_ = focused; }
    [[nodiscard]] bool focused() const override { return focused_; }

    std::vector<std::string> inputs{};

private:
    bool focused_{false};
};

} // namespace

TEST_CASE("scroll viewports follow the content end and report unused deltas as the frozen pi frame does",
        "[tui][scroll-view][issue1000][compat-pi]") {
    const auto bundle = named_bundle();
    REQUIRE(bundle);
    const auto& scenario = scenario_of(*bundle, "follow-transitions");
    const auto& frozen = scenario.at("expected");
    const auto& window = frozen.at("window");
    const auto& requests = scenario.at("inputs").at("requests").get_array();

    cch::tui::ScrollView view(std::make_unique<cch::tui::Text>(numbered_text(6), 0, 0),
            cch::tui::ScrollViewOptions{.follow_end = true, .primary = true});
    CHECK(view.follow_end());
    CHECK(view.primary());

    std::size_t index = 0;
    for (const auto& step : frozen.at("steps").get_array()) {
        REQUIRE(index < requests.size());
        if (!requests[index].holds<JsonValue::null_t>())
            CHECK(view.scroll_by(int_of(requests[index])) == int_of(step.at("remaining")));
        const auto frame = cch::tui::detail::LayoutEngine::render(view, 10, 3);
        REQUIRE(frame);
        CHECK(view.scroll_top() == static_cast<std::size_t>(int_of(step.at("scrollTop"))));
        CHECK(view.following_end() == step.at("followingEnd").get_boolean());
        CHECK(view.viewport_height() == static_cast<std::size_t>(int_of(step.at("viewportHeight"))));
        CHECK(view.content_height() == static_cast<std::size_t>(int_of(window.at("contentHeight"))));
        CHECK(drawn_lines(*frame) == string_lines(step.at("lines")));
        ++index;
    }
    CHECK(index == requests.size());
}

TEST_CASE("appended rows move a followed viewport as the frozen pi frame does",
        "[tui][scroll-view][issue1000][compat-pi]") {
    const auto bundle = named_bundle();
    REQUIRE(bundle);
    const auto& scenario = scenario_of(*bundle, "follow-append");
    const auto& frozen = scenario.at("expected");
    const auto& inputs = scenario.at("inputs");

    auto text = std::make_unique<cch::tui::Text>(
            numbered_text(static_cast<std::size_t>(int_of(inputs.at("contentLines")))), 0, 0);
    auto* text_pointer = text.get();
    cch::tui::ScrollView view(std::move(text), cch::tui::ScrollViewOptions{.follow_end = true});

    auto frame = cch::tui::detail::LayoutEngine::render(view, 6, 3);
    REQUIRE(frame);
    CHECK(view.scroll_top() == static_cast<std::size_t>(int_of(frozen.at("before").at("scrollTop"))));
    CHECK(view.following_end() == frozen.at("before").at("followingEnd").get_boolean());
    CHECK(view.viewport_height() == static_cast<std::size_t>(int_of(frozen.at("before").at("viewportHeight"))));
    CHECK(view.content_height() == static_cast<std::size_t>(int_of(frozen.at("window").at("contentHeight"))));
    CHECK(drawn_lines(*frame) == string_lines(frozen.at("before").at("lines")));

    text_pointer->set_text(numbered_text(static_cast<std::size_t>(int_of(inputs.at("appendedLines")))));
    frame = cch::tui::detail::LayoutEngine::render(view, 6, 3);
    REQUIRE(frame);
    CHECK(view.scroll_top() == static_cast<std::size_t>(int_of(frozen.at("after").at("scrollTop"))));
    CHECK(view.following_end() == frozen.at("after").at("followingEnd").get_boolean());
    CHECK(view.viewport_height() == static_cast<std::size_t>(int_of(frozen.at("after").at("viewportHeight"))));
    CHECK(drawn_lines(*frame) == string_lines(frozen.at("after").at("lines")));
}

TEST_CASE("an absolute scroll with disableFollow holds the last row until follow is restored",
        "[tui][scroll-view][issue1000][compat-pi]") {
    const auto bundle = named_bundle();
    REQUIRE(bundle);
    const auto& scenario = scenario_of(*bundle, "scroll-to-disable-follow");
    const auto& frozen = scenario.at("expected");
    const auto& inputs = scenario.at("inputs");
    const auto& actions = inputs.at("actions").get_array();

    auto text = std::make_unique<cch::tui::Text>(
            numbered_text(static_cast<std::size_t>(int_of(inputs.at("contentLines")))), 0, 0);
    auto* text_pointer = text.get();
    cch::tui::ScrollView view(std::move(text), cch::tui::ScrollViewOptions{.follow_end = true});

    std::size_t index = 0;
    for (const auto& phase : frozen.at("phases").get_array()) {
        REQUIRE(index < actions.size());
        const auto& action = actions[index];
        const auto name = action.at("action").get_string();
        CHECK(name == phase.at("action").get_string());
        if (name == "appended-while-suppressed") {
            // The suppressed window keeps its rows while the content grows.
            text_pointer->set_text(numbered_text(static_cast<std::size_t>(int_of(inputs.at("appendedLines")))));
        } else if (!action.at("target").holds<JsonValue::null_t>()) {
            view.scroll_to(int_of(action.at("target")), action.at("disableFollow").get_boolean());
        }
        const auto frame = cch::tui::detail::LayoutEngine::render(view, 10, 3);
        REQUIRE(frame);
        CHECK(view.scroll_top() == static_cast<std::size_t>(int_of(phase.at("scrollTop"))));
        CHECK(view.following_end() == phase.at("followingEnd").get_boolean());
        CHECK(view.viewport_height() == static_cast<std::size_t>(int_of(phase.at("viewportHeight"))));
        CHECK(view.content_height() == static_cast<std::size_t>(int_of(phase.at("contentHeight"))));
        CHECK(drawn_lines(*frame) == string_lines(phase.at("lines")));
        ++index;
    }
    CHECK(index == actions.size());
}

TEST_CASE("nested viewport measurement, delta ownership and depth order match the frozen pi frame",
        "[tui][scroll-view][issue1000][compat-pi]") {
    const auto bundle = named_bundle();
    REQUIRE(bundle);
    const auto& scenario = scenario_of(*bundle, "nested-scroll");
    const auto& frozen = scenario.at("expected");
    const auto& inputs = scenario.at("inputs");

    auto inner = std::make_unique<cch::tui::ScrollView>(std::make_unique<cch::tui::Text>(
            numbered_text(static_cast<std::size_t>(int_of(inputs.at("inner").at("contentLines")))), 0, 0));
    auto* inner_pointer = inner.get();
    cch::tui::VStack content;
    REQUIRE(content.add_child(std::move(inner),
            cch::tui::StackEntryOptions{
                    .basis = static_cast<std::size_t>(int_of(inputs.at("outer").at("innerBasis")))}));
    REQUIRE(content.add_child(std::make_unique<cch::tui::Text>(inputs.at("outer").at("tail").get_string(), 0, 0)));
    cch::tui::ScrollView outer(std::make_unique<cch::tui::VStack>(std::move(content)));

    auto frame = cch::tui::detail::LayoutEngine::render(outer, 10, 2);
    REQUIRE(frame);
    const auto& before = frozen.at("before");
    CHECK(inner_pointer->viewport_height() == static_cast<std::size_t>(int_of(before.at("innerViewportHeight"))));
    CHECK(inner_pointer->scroll_top() == static_cast<std::size_t>(int_of(before.at("innerScrollTop"))));
    CHECK(outer.scroll_top() == static_cast<std::size_t>(int_of(before.at("outerScrollTop"))));
    CHECK(outer.viewport_height() == static_cast<std::size_t>(int_of(before.at("outerViewportHeight"))));
    CHECK(outer.content_height() == static_cast<std::size_t>(int_of(frozen.at("window").at("contentHeight"))));
    CHECK(drawn_lines(*frame) == string_lines(before.at("lines")));

    const auto& after = frozen.at("after");
    CHECK(outer.scroll_by(10) == int_of(after.at("remaining")));
    CHECK(outer.scroll_top() == static_cast<std::size_t>(int_of(after.at("outerScrollTop"))));
    frame = cch::tui::detail::LayoutEngine::render(outer, 10, 2);
    REQUIRE(frame);
    CHECK(drawn_lines(*frame) == string_lines(after.at("lines")));

    // The viewports under a frame cell, deepest first (pi `layout.ts:439-449`).
    const auto& probes = scenario.at("inputs").at("probeDimensions");
    const auto probe_frame = cch::tui::detail::LayoutEngine::render(outer,
            static_cast<std::size_t>(int_of(probes.at("columns"))),
            static_cast<std::size_t>(int_of(probes.at("rows"))));
    REQUIRE(probe_frame);
    std::size_t probe_index = 0;
    for (const auto& probe : frozen.at("viewportsAt").get_array()) {
        const auto viewports =
                cch::tui::detail::scroll_views_at(*probe_frame, int_of(probe.at("x")), int_of(probe.at("y")));
        std::vector<std::string> names;
        for (auto* viewport : viewports) {
            if (viewport == inner_pointer)
                names.emplace_back("inner");
            else if (viewport == &outer)
                names.emplace_back("outer");
            else
                names.emplace_back("unknown");
        }
        CHECK(names == string_lines(probe.at("depthFirst")));
        ++probe_index;
    }
    CHECK(probe_index == 3);
}

TEST_CASE("scrolled image placements match the frozen pi crop geometry", "[tui][scroll-view][issue1000][compat-pi]") {
    const auto bundle = named_bundle();
    REQUIRE(bundle);
    const auto& scenario = scenario_of(*bundle, "scroll-image-crop");
    const auto& frozen = scenario.at("expected");
    const auto& inputs = scenario.at("inputs");
    const auto placed_columns = static_cast<std::size_t>(int_of(inputs.at("placement").at("columns")));
    const auto placed_rows = static_cast<std::size_t>(int_of(inputs.at("placement").at("rows")));
    const auto height_px = int_of(inputs.at("placement").at("heightPx"));

    /// pi's own protocol crop of a placement the window cut: the source
    /// rectangle follows `cropKittyImageLine` (`terminal-image.ts:452-463`) from
    /// the hidden and visible rows, so the C++ metadata is what #1012 needs.
    const auto source_of = [height_px, placed_rows](std::ptrdiff_t hidden_rows, std::ptrdiff_t visible_rows) {
        const auto source_y = static_cast<std::ptrdiff_t>(std::floor(
                static_cast<double>(height_px) * static_cast<double>(hidden_rows) / static_cast<double>(placed_rows)));
        const auto source_end = static_cast<std::ptrdiff_t>(
                std::ceil(static_cast<double>(height_px) * static_cast<double>(hidden_rows + visible_rows) /
                          static_cast<double>(placed_rows)));
        return std::pair{
                source_y, std::max<std::ptrdiff_t>(1, std::min<std::ptrdiff_t>(height_px, source_end) - source_y)};
    };

    // Lower boundary: the placement sits on the transcript's third row below a
    // dock row, so its last row falls outside the window.
    {
        const auto& observed = frozen.at("lower");
        const auto placement_row = static_cast<std::size_t>(int_of(observed.at("placementRow")));
        cch::tui::VStack stack;
        REQUIRE(stack.add_child(std::make_unique<ImageChild>(std::vector<std::string>{"one", "two", "", "", ""},
                                        placement(placement_row, placed_columns, placed_rows)),
                cch::tui::StackEntryOptions{.basis = 0, .grow = 1}));
        REQUIRE(stack.add_child(
                std::make_unique<cch::tui::Text>(inputs.at("lowerBoundary").at("dock").get_string(), 0, 0)));
        auto frame = cch::tui::detail::LayoutEngine::render(stack, 20, 4);
        REQUIRE(frame);
        REQUIRE(frame->images.size() == 1);
        const auto& image = frame->images.front();
        CHECK(image.region.row == static_cast<std::size_t>(int_of(observed.at("cropRow"))));
        CHECK(image.region.rows == static_cast<std::size_t>(int_of(observed.at("crop").at("r"))));
        CHECK(image.source_row_offset == 0);
        const auto [source_y, source_h] = source_of(0, static_cast<std::ptrdiff_t>(image.region.rows));
        CHECK(source_y == int_of(observed.at("crop").at("y")));
        CHECK(source_h == int_of(observed.at("crop").at("h")));
        CHECK(image.region.columns ==
                static_cast<std::size_t>(int_of(observed.at("crop").at("registered").at("columns"))));
    }

    // Scrolled: the placement sits on the first content row and the window
    // follows the end, so its remaining row is painted at the window's top.
    {
        const auto& observed = frozen.at("upper");
        const auto placement_row = static_cast<std::size_t>(int_of(observed.at("placementRow")));
        cch::tui::ScrollView view(
                std::make_unique<ImageChild>(std::vector<std::string>{"", "", "", "turns-4", "turns-5"},
                        placement(placement_row, placed_columns, placed_rows)),
                cch::tui::ScrollViewOptions{.follow_end = true});
        auto frame = cch::tui::detail::LayoutEngine::render(view, 20, 3);
        REQUIRE(frame);
        CHECK(view.scroll_top() == static_cast<std::size_t>(int_of(observed.at("scrollTop"))));
        CHECK(view.viewport_height() == static_cast<std::size_t>(int_of(observed.at("viewportHeight"))));
        REQUIRE(frame->images.size() == 1);
        const auto& image = frame->images.front();
        const auto hidden_rows = int_of(observed.at("scrollTop")) - int_of(observed.at("placementRow"));
        CHECK(hidden_rows == 2);
        CHECK(image.region.row == static_cast<std::size_t>(int_of(observed.at("cropRow"))));
        CHECK(image.region.rows == static_cast<std::size_t>(int_of(observed.at("crop").at("r"))));
        CHECK(image.source_row_offset == static_cast<std::size_t>(hidden_rows));
        const auto [source_y, source_h] = source_of(hidden_rows, static_cast<std::ptrdiff_t>(image.region.rows));
        CHECK(source_y == int_of(observed.at("crop").at("y")));
        CHECK(source_h == int_of(observed.at("crop").at("h")));
    }
}

TEST_CASE("the implicit alternate-screen viewport matches the frozen pi phases",
        "[tui][alt-screen][scroll-view][issue1000][compat-pi]") {
    const auto bundle = named_bundle();
    REQUIRE(bundle);
    const auto& scenario = scenario_of(*bundle, "alt-screen-implicit-follow");
    const auto& phases = scenario.at("expected").at("phases").get_array();
    const auto& inputs = scenario.at("inputs");
    const auto resizes = scenario.at("inputs").at("resizes").get_array();

    cch::tui::VirtualTerminal terminal({.columns = 20, .rows = 4});
    cch::tui::AltScreen tui(terminal);
    auto text = std::make_unique<cch::tui::Text>(
            numbered_text(static_cast<std::size_t>(int_of(inputs.at("documentLines")))), 0, 0);
    auto* text_pointer = text.get();
    REQUIRE(tui.add_child(std::move(text)));
    REQUIRE(tui.start());

    std::size_t phase_index = 0;
    const auto check_phase = [&](const JsonValue& phase) {
        CHECK(tui.viewport_top() == static_cast<std::size_t>(int_of(phase.at("viewportTop"))));
        CHECK(tui.is_following_output() == phase.at("isFollowingOutput").get_boolean());
        CHECK(trimmed_rows(terminal) == string_lines(phase.at("viewport")));
    };

    REQUIRE(phases.size() == 7);
    REQUIRE(tui.render());
    check_phase(phases[phase_index++]);
    tui.scroll_by(int_of(inputs.at("manualDelta")));
    REQUIRE(tui.render());
    check_phase(phases[phase_index++]);
    text_pointer->set_text(numbered_text(static_cast<std::size_t>(int_of(inputs.at("appendedLines")))));
    REQUIRE(tui.render());
    check_phase(phases[phase_index++]);
    tui.scroll_to_bottom();
    REQUIRE(tui.render());
    check_phase(phases[phase_index++]);
    REQUIRE(terminal.inject_resize(cch::tui::TerminalDimensions{
            .columns = 20, .rows = static_cast<std::size_t>(int_of(resizes[0].at("rows")))}));
    REQUIRE(tui.render());
    check_phase(phases[phase_index++]);
    tui.scroll_to_top();
    REQUIRE(tui.render());
    check_phase(phases[phase_index++]);
    REQUIRE(terminal.inject_resize(cch::tui::TerminalDimensions{
            .columns = 20, .rows = static_cast<std::size_t>(int_of(resizes[1].at("rows")))}));
    REQUIRE(tui.render());
    check_phase(phases[phase_index++]);
    CHECK(phase_index == phases.size());
}

TEST_CASE("frozen wheel policy drives fixed, accelerated, burst, reset, and fractional deltas",
        "[tui][scroll-view][issue1004][compat-pi]") {
    const auto bundle = named_bundle();
    REQUIRE(bundle);
    const auto& scenario = scenario_of(*bundle, "wheel-accelerator");
    const auto& inputs = scenario.at("inputs");
    const auto& expected = scenario.at("expected");
    auto clock = std::chrono::steady_clock::time_point{};
    const auto exercise = [&](cch::tui::WheelScrollLines policy,
                                  const JsonValue& stream,
                                  std::string_view expected_key) {
        std::vector<std::ptrdiff_t> observed;
        auto region = std::make_unique<cch::tui::MouseRegion>(std::make_unique<cch::tui::Text>(numbered_text(80), 0, 0),
                [&observed](const cch::tui::MouseEvent& event) -> std::optional<cch::tui::MouseEventResult> {
                    if (event.type != cch::tui::MouseEventType::Wheel) return std::nullopt;
                    observed.push_back(event.wheel_delta.value_or(0));
                    return cch::tui::MouseEventResult{.handled = true, .render = false};
                });
        cch::tui::VirtualTerminal terminal({.columns = 20, .rows = 4});
        cch::tui::AltScreen tui(terminal,
                cch::tui::AltScreenOptions{
                        .wheel_scroll_lines = std::move(policy), .wheel_clock = [&clock] { return clock; }});
        REQUIRE(tui.add_child(std::move(region)));
        REQUIRE(tui.start());
        REQUIRE(tui.render());
        const auto& times = stream.at("times").get_array();
        const auto direction_it = stream.get_object().find("directions");
        const auto* directions =
                direction_it == stream.get_object().end() ? nullptr : direction_it->second.get_if<JsonValue::array_t>();
        std::size_t index = 0;
        for (const auto& time : times) {
            clock = std::chrono::steady_clock::time_point{} +
                    std::chrono::milliseconds{static_cast<std::ptrdiff_t>(time.get_number())};
            const bool up = directions != nullptr && int_of((*directions)[index]) < 0;
            REQUIRE(terminal.inject_input(up ? "\x1b[<64;1;1M" : "\x1b[<65;1;1M"));
            ++index;
        }
        REQUIRE(tui.stop());
        std::vector<std::ptrdiff_t> frozen;
        const auto& frozen_values = expected.at(std::string{expected_key}).get_array();
        for (std::size_t value_index = 0; value_index < frozen_values.size(); ++value_index) {
            auto value = int_of(frozen_values[value_index]);
            if (directions != nullptr) value *= int_of((*directions)[value_index]);
            frozen.push_back(value);
        }
        CHECK(observed == frozen);
        return observed;
    };

    const auto& fixed = inputs.at("fixed");
    CHECK(exercise(cch::tui::WheelScrollLines{static_cast<std::size_t>(int_of(fixed.at("lines")))}, fixed, "fixed") ==
            std::vector<std::ptrdiff_t>{3, 3, 3, 3});
    CHECK(exercise(cch::tui::AutoWheelScrollLines{}, inputs.at("auto"), "accelerated") ==
            std::vector<std::ptrdiff_t>{1, 2, 2, 1, 2, 1});
    CHECK(exercise(cch::tui::AutoWheelScrollLines{}, inputs.at("directionReset"), "directionReset") ==
            std::vector<std::ptrdiff_t>{1, 5, 5, -1, 1, 5});
    CHECK(exercise(cch::tui::AutoWheelScrollLines{}, inputs.at("fractionalCarry"), "fractionalCarry") ==
            std::vector<std::ptrdiff_t>{1, 2, 3, 2, 3});

    std::vector<std::ptrdiff_t> alt_deltas;
    auto alt_region = std::make_unique<cch::tui::MouseRegion>(std::make_unique<cch::tui::Text>(numbered_text(20), 0, 0),
            [&alt_deltas](const cch::tui::MouseEvent& event) -> std::optional<cch::tui::MouseEventResult> {
                if (event.type != cch::tui::MouseEventType::Wheel) return std::nullopt;
                alt_deltas.push_back(event.wheel_delta.value_or(0));
                return cch::tui::MouseEventResult{.handled = true, .render = false};
            });
    cch::tui::VirtualTerminal alt_terminal({.columns = 20, .rows = 4});
    cch::tui::AltScreen alt_tui(alt_terminal, cch::tui::AltScreenOptions{.wheel_scroll_lines = std::size_t{3}});
    REQUIRE(alt_tui.add_child(std::move(alt_region)));
    REQUIRE(alt_tui.start());
    REQUIRE(alt_tui.render());
    REQUIRE(alt_terminal.inject_input("\x1b[<72;1;1M"));
    REQUIRE(alt_tui.stop());
    CHECK(alt_deltas == std::vector<std::ptrdiff_t>{-15});
}

TEST_CASE("frozen nested wheel routing consumes the child then chains only to its enclosing viewport",
        "[tui][scroll-view][issue1004][compat-pi]") {
    const auto bundle = named_bundle();
    REQUIRE(bundle);
    const auto& scenario = scenario_of(*bundle, "nested-wheel-chaining");
    const auto& expected = scenario.at("expected");
    cch::tui::VirtualTerminal terminal({.columns = 30, .rows = 3});
    cch::tui::AltScreen tui(terminal, cch::tui::AltScreenOptions{.wheel_scroll_lines = std::size_t{5}});
    tui.set_keybinding_context(std::make_shared<cch::tui::KeybindingsContext>(fullscreen_registry()));
    auto inner = std::make_unique<cch::tui::ScrollView>(std::make_unique<cch::tui::Text>(numbered_text(4), 0, 0));
    auto* inner_ptr = inner.get();
    cch::tui::VStack left;
    REQUIRE(left.add_child(std::make_unique<cch::tui::Text>("head", 0, 0), cch::tui::StackEntryOptions{.basis = 1}));
    REQUIRE(left.add_child(std::move(inner), cch::tui::StackEntryOptions{.basis = 2}));
    REQUIRE(left.add_child(std::make_unique<cch::tui::Text>("tail", 0, 0), cch::tui::StackEntryOptions{.basis = 5}));
    auto outer = std::make_unique<cch::tui::ScrollView>(
            std::make_unique<cch::tui::VStack>(std::move(left)), cch::tui::ScrollViewOptions{.primary = true});
    auto* outer_ptr = outer.get();
    auto sibling = std::make_unique<cch::tui::ScrollView>(std::make_unique<cch::tui::Text>(numbered_text(10), 0, 0));
    auto* sibling_ptr = sibling.get();
    cch::tui::HStack root;
    REQUIRE(root.add_child(std::move(outer), cch::tui::StackEntryOptions{.basis = 15}));
    REQUIRE(root.add_child(std::move(sibling), cch::tui::StackEntryOptions{.basis = 15}));
    tui.set_layout_root(std::make_unique<cch::tui::HStack>(std::move(root)));
    REQUIRE(tui.start());
    REQUIRE(tui.render());
    CHECK(static_cast<std::ptrdiff_t>(inner_ptr->scroll_top()) == int_of(expected.at("before").at("inner")));
    CHECK(static_cast<std::ptrdiff_t>(outer_ptr->scroll_top()) == int_of(expected.at("before").at("outer")));
    CHECK(static_cast<std::ptrdiff_t>(sibling_ptr->scroll_top()) == int_of(expected.at("before").at("unrelated")));
    REQUIRE(terminal.inject_input(std::string{scenario.at("inputs").at("wheelReport").get_string()}));
    REQUIRE(tui.render());
    CHECK(static_cast<std::ptrdiff_t>(inner_ptr->scroll_top()) == int_of(expected.at("after").at("inner")));
    CHECK(static_cast<std::ptrdiff_t>(outer_ptr->scroll_top()) == int_of(expected.at("after").at("outer")));
    CHECK(static_cast<std::ptrdiff_t>(sibling_ptr->scroll_top()) == int_of(expected.at("after").at("unrelated")));
    REQUIRE(tui.stop());
}

TEST_CASE("frozen fullscreen navigation, releases, and indicator clicks match pi",
        "[tui][scroll-view][issue1004][compat-pi]") {
    const auto bundle = named_bundle();
    REQUIRE(bundle);
    const auto& scenario = scenario_of(*bundle, "alt-screen-navigation-indicator");
    const auto& phases = scenario.at("expected").at("phases").get_array();
    const auto phase = [&](std::string_view name) -> const JsonValue& {
        for (const auto& value : phases)
            if (value.at("action").get_string() == name) return value;
        REQUIRE(false);
        return phases.front();
    };
    const auto& inputs = scenario.at("inputs");
    cch::tui::VirtualTerminal terminal({.columns = 20,
            .rows = 4,
            .capabilities = {.synchronized_output = true, .keyboard_protocol = cch::tui::KeyboardProtocol::Kitty}});
    cch::tui::AltScreen tui(
            terminal, cch::tui::AltScreenOptions{.scroll_to_end_indicator = [] { return std::string{"Jump end"}; }});
    auto transcript = std::make_unique<cch::tui::ScrollView>(std::make_unique<cch::tui::Text>(numbered_text(12), 0, 0),
            cch::tui::ScrollViewOptions{.follow_end = true, .primary = true});
    auto* transcript_ptr = transcript.get();
    auto editor = std::make_unique<InputProbe>();
    auto* editor_ptr = editor.get();
    cch::tui::VStack root;
    REQUIRE(root.add_child(std::move(transcript), cch::tui::StackEntryOptions{.basis = 0, .grow = 1}));
    REQUIRE(root.add_child(std::move(editor), cch::tui::StackEntryOptions{.basis = 1, .shrink = 0}));
    tui.set_layout_root(std::make_unique<cch::tui::VStack>(std::move(root)));
    auto context = std::make_shared<cch::tui::KeybindingsContext>(fullscreen_registry());
    tui.set_keybinding_context(context);
    REQUIRE(tui.set_focus(editor_ptr));
    REQUIRE(tui.start());
    REQUIRE(tui.render());
    CHECK(static_cast<std::ptrdiff_t>(transcript_ptr->scroll_top()) == int_of(phase("initial").at("scrollTop")));
    for (const auto& value : inputs.at("navigation").get_array())
        REQUIRE(terminal.inject_input(value.get_string()));
    REQUIRE(tui.render());
    CHECK(static_cast<std::ptrdiff_t>(transcript_ptr->scroll_top()) ==
            int_of(phase("ctrl-home-release").at("scrollTop")));
    std::vector<std::string> expected_editor_inputs;
    for (const auto& value : scenario.at("expected").at("editorInputs").get_array())
        expected_editor_inputs.push_back(value.get_string());
    CHECK(editor_ptr->inputs == expected_editor_inputs);
    REQUIRE(terminal.inject_input("\x1b[1;5H"));
    REQUIRE(tui.render());
    const auto& home = phase("ctrl-home");
    CHECK(static_cast<std::ptrdiff_t>(transcript_ptr->scroll_top()) == int_of(home.at("scrollTop")));
    CHECK(transcript_ptr->following_end() == home.at("followingEnd").get_boolean());
    for (std::size_t row = 0; row < home.at("lines").get_array().size(); ++row) {
        auto actual = terminal.screen()[row];
        while (!actual.empty() && actual.back() == ' ')
            actual.pop_back();
        CHECK(actual == home.at("lines").get_array()[row].get_string());
    }
    REQUIRE(terminal.inject_input("\x1b[1;5F"));
    REQUIRE(tui.render());
    const auto& end = phase("ctrl-end");
    CHECK(static_cast<std::ptrdiff_t>(transcript_ptr->scroll_top()) == int_of(end.at("scrollTop")));
    CHECK(transcript_ptr->following_end() == end.at("followingEnd").get_boolean());
    REQUIRE(terminal.inject_input("\x1b[<64;1;1M"));
    REQUIRE(tui.render());
    const auto& wheel = phase("wheel-up-indicator");
    CHECK(static_cast<std::ptrdiff_t>(transcript_ptr->scroll_top()) == int_of(wheel.at("scrollTop")));
    CHECK(transcript_ptr->following_end() == wheel.at("followingEnd").get_boolean());
    CHECK(terminal.screen()[2].find("Jump end") != std::string::npos);
    REQUIRE(terminal.inject_input("\x1b[<0;7;3M"));
    REQUIRE(tui.render());
    const auto& clicked = phase("indicator-click");
    CHECK(static_cast<std::ptrdiff_t>(transcript_ptr->scroll_top()) == int_of(clicked.at("scrollTop")));
    CHECK(transcript_ptr->following_end() == clicked.at("followingEnd").get_boolean());
    REQUIRE(tui.stop());
}

TEST_CASE("a primary scroll view without follow-end never shows the jump-to-end indicator",
        "[tui][alt-screen][scroll-view][issue1004][compat-pi]") {
    const auto bundle = named_bundle();
    REQUIRE(bundle);
    const auto& scenario = scenario_of(*bundle, "alt-screen-indicator-no-follow-end");
    const auto& phases = scenario.at("expected").at("phases").get_array();
    const auto phase = [&](std::string_view name) -> const JsonValue& {
        for (const auto& value : phases)
            if (value.at("action").get_string() == name) return value;
        REQUIRE(false);
        return phases.front();
    };
    cch::tui::VirtualTerminal terminal({.columns = 30, .rows = 3});
    cch::tui::AltScreen tui(terminal, cch::tui::AltScreenOptions{.scroll_to_end_indicator = [] {
        return std::string{" \xe2\x86\x93 Jump to end "};
    }});
    auto transcript = std::make_unique<cch::tui::ScrollView>(
            std::make_unique<cch::tui::Text>("one\ntwo\nthree\nfour\nfive", 0, 0),
            cch::tui::ScrollViewOptions{.primary = true});
    auto* transcript_ptr = transcript.get();
    tui.set_layout_root(std::move(transcript));
    REQUIRE(tui.start());
    REQUIRE(tui.render());
    const auto& initial = phase("initial");
    CHECK(static_cast<std::ptrdiff_t>(transcript_ptr->scroll_top()) == int_of(initial.at("scrollTop")));
    CHECK(transcript_ptr->following_end() == initial.at("followingEnd").get_boolean());
    [[maybe_unused]] const auto scrolled = transcript_ptr->scroll_by(
            int_of(phase("scrolled-away-from-end").at("scrollTop")) - int_of(initial.at("scrollTop")));
    REQUIRE(tui.render());
    const auto& away = phase("scrolled-away-from-end");
    CHECK(static_cast<std::ptrdiff_t>(transcript_ptr->scroll_top()) == int_of(away.at("scrollTop")));
    // The frozen negative claim: scrolled away from the end, the indicator
    // label must not appear anywhere in the viewport.
    for (std::size_t row = 0; row < away.at("lines").get_array().size(); ++row) {
        auto actual = terminal.screen()[row];
        while (!actual.empty() && actual.back() == ' ')
            actual.pop_back();
        CHECK(actual == away.at("lines").get_array()[row].get_string());
        CHECK(actual.find("Jump to end") == std::string::npos);
    }
    REQUIRE(tui.stop());
}

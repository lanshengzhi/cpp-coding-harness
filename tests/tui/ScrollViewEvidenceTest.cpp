// Named pi-v1.0.4 acceptance for scroll viewports (#1000). Every expectation is
// read from the verified named bundle through `scripts/tui/evidence.py`; nothing
// here regenerates expectations from Pike or falls back to the historical
// `fixtures/pi-tui/*.json` root snapshots.

#include <cch/tui/AltScreen.hpp>
#include <cch/tui/ScrollView.hpp>
#include <cch/tui/Stack.hpp>
#include <cch/tui/Text.hpp>
#include <cch/tui/VStack.hpp>
#include <cch/tui/VirtualTerminal.hpp>

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

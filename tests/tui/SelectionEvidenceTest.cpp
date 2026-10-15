// Named pi-v1.0.4 acceptance for fullscreen text selection (#1006),
// multi-click selection (#1007) and mouse action routing (#1009). Expectations
// from the verified frozen bundle; the cases exercise the public AltScreen,
// Text, ScrollView and VirtualTerminal seams.

#include <cch/tui/AltScreen.hpp>
#include <cch/tui/ScrollView.hpp>
#include <cch/tui/Text.hpp>
#include <cch/tui/VirtualTerminal.hpp>

#include "support/PiTuiEvidence.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

using cch::support::JsonValue;

[[nodiscard]] const JsonValue& scenario_of(const JsonValue& bundle, std::string_view name) {
    for (const auto& scenario : bundle.at("scenarios").get_array()) {
        if (scenario.at("name").get_string() == name) return scenario;
    }
    FAIL("missing frozen selection scenario");
    return bundle;
}

void drag(cch::tui::VirtualTerminal& terminal, const JsonValue& inputs) {
    const auto& points = inputs.at("drag").get_array();
    const auto& start = points.at(0);
    const auto& end = points.at(1);
    const auto coordinate = [](const JsonValue& point, std::string_view axis) {
        return static_cast<std::size_t>(point.at(std::string{axis}).get_number());
    };
    const auto x1 = coordinate(start, "x");
    const auto y1 = coordinate(start, "y");
    const auto x2 = coordinate(end, "x");
    const auto y2 = coordinate(end, "y");
    REQUIRE(terminal.inject_input("\x1b[<0;" + std::to_string(x1) + ";" + std::to_string(y1) + "M"));
    REQUIRE(terminal.inject_input("\x1b[<32;" + std::to_string(x2) + ";" + std::to_string(y2) + "M"));
    REQUIRE(terminal.inject_input("\x1b[<0;" + std::to_string(x2) + ";" + std::to_string(y2) + "m"));
}

[[nodiscard]] cch::support::Expected<JsonValue> selection_bundle() {
    return cch::tests::read_pi_tui_evidence("selection.json");
}

[[nodiscard]] const JsonValue& mouse_scenario_of(const JsonValue& bundle, std::string_view name) {
    for (const auto& scenario : bundle.at("scenarios").get_array()) {
        if (scenario.at("name").get_string() == name) return scenario;
    }
    FAIL("missing frozen mouse routing scenario");
    return bundle;
}

[[nodiscard]] std::vector<std::string> copied_texts(const JsonValue& values) {
    std::vector<std::string> texts;
    for (const auto& value : values.get_array())
        texts.push_back(value.get_string());
    return texts;
}

/// The visible rows the way the frozen capture records them: ANSI sequences
/// stripped and trailing whitespace trimmed (pi `stripTerminalSequences` +
/// `trimEnd`), which keeps the scrollbar glyphs that end a padded row.
[[nodiscard]] std::vector<std::string> observed_rows(const cch::tui::VirtualTerminal& terminal) {
    std::vector<std::string> rows;
    for (const auto& row : terminal.screen()) {
        auto visible = cch::tui::strip_terminal_sequences(row);
        while (!visible.empty() && (visible.back() == ' ' || visible.back() == '\t'))
            visible.pop_back();
        rows.push_back(std::move(visible));
    }
    return rows;
}

/// Replay one frozen #1007 multi-click scenario. The recorded steps carry
/// `advanceMs` clock deltas; the replay injects them on a deterministic
/// selection-click clock and compares every recorded observation (selection
/// state plus the accumulated copies) and viewport snapshot in order.
void replay_multiclick_scenario(std::string_view name) {
    const auto bundle = selection_bundle();
    REQUIRE(bundle);
    const auto& scenario = scenario_of(*bundle, name);
    const auto& dimensions = scenario.at("dimensions");
    const auto& inputs = scenario.at("inputs");
    const auto& expected = scenario.at("expected");
    cch::tui::VirtualTerminal terminal({
            .columns = static_cast<std::size_t>(dimensions.at("columns").get_number()),
            .rows = static_cast<std::size_t>(dimensions.at("rows").get_number()),
    });
    cch::tui::AltScreen tui(terminal);
    std::vector<std::string> copied;
    tui.set_selection_copy_sink([&copied](std::string_view text) -> cch::support::ExpectedVoid {
        copied.emplace_back(text);
        return {};
    });
    std::chrono::milliseconds now{0};
    tui.set_selection_click_clock([&now] { return now; });

    if (const auto* object = inputs.get_if<JsonValue::object_t>();
            object != nullptr && object->contains("layoutText")) {
        const auto& scroll = inputs.at("layoutScroll");
        tui.set_layout_root(std::make_unique<cch::tui::ScrollView>(
                std::make_unique<cch::tui::Text>(inputs.at("layoutText").get_string(), 0, 0),
                cch::tui::ScrollViewOptions{.follow_end = scroll.at("follow").get_string() == "end",
                        .primary = scroll.at("primary").get_boolean(),
                        .scrollbar = cch::tui::ScrollbarMode::Always,
                        .scrollbar_hide_delay = std::chrono::milliseconds{
                                static_cast<int>(scroll.at("scrollbarHideDelayMs").get_number())}}));
    } else {
        REQUIRE(tui.add_child(std::make_unique<cch::tui::Text>(inputs.at("text").get_string(), 0, 0)));
    }
    REQUIRE(tui.start());
    // The selection source rows fill during a rendered frame; every frozen
    // scenario observes clicks against the first rendered content.
    REQUIRE(tui.render());

    struct Observation {
        bool has_selection{false};
        std::size_t copied_count{0};
    };
    std::vector<Observation> observations;
    std::vector<std::vector<std::string>> viewports;
    for (const auto& step : inputs.at("steps").get_array()) {
        const auto& action = step.at("action").get_string();
        const auto has_coordinate = action == "press" || action == "release" || action == "motion";
        const auto x = has_coordinate ? static_cast<std::ptrdiff_t>(step.at("x").get_number()) : 0;
        const auto y = has_coordinate ? static_cast<std::ptrdiff_t>(step.at("y").get_number()) : 0;
        if (action == "press") {
            now += std::chrono::milliseconds{static_cast<int>(step.at("advanceMs").get_number())};
            REQUIRE(terminal.inject_input("\x1b[<0;" + std::to_string(x) + ";" + std::to_string(y) + "M"));
        } else if (action == "release") {
            REQUIRE(terminal.inject_input("\x1b[<0;" + std::to_string(x) + ";" + std::to_string(y) + "m"));
        } else if (action == "motion") {
            REQUIRE(terminal.inject_input("\x1b[<32;" + std::to_string(x) + ";" + std::to_string(y) + "M"));
        } else if (action == "focusOut") {
            REQUIRE(terminal.inject_input("\x1b[O"));
        } else if (action == "focusIn") {
            REQUIRE(terminal.inject_input("\x1b[I"));
        } else if (action == "observe") {
            observations.push_back(
                    Observation{.has_selection = tui.has_active_selection(), .copied_count = copied.size()});
        } else if (action == "observeViewport") {
            viewports.push_back(observed_rows(terminal));
        }
        // pi's capture waits for the pending frame after every step; the
        // toolkit without a scheduler only queues one, so pump it here.
        REQUIRE(tui.render());
    }

    const auto& frozen_observations = expected.at("observations").get_array();
    REQUIRE(observations.size() == frozen_observations.size());
    for (std::size_t index = 0; index < observations.size(); ++index) {
        INFO("observation " << index);
        const auto& frozen = frozen_observations[index];
        CHECK(observations[index].has_selection == frozen.at("hasSelection").get<bool>());
        const auto frozen_copies = copied_texts(frozen.at("copied"));
        REQUIRE(observations[index].copied_count == frozen_copies.size());
        CHECK(std::equal(frozen_copies.begin(), frozen_copies.end(), copied.begin()));
    }
    if (const auto* object = expected.get_if<JsonValue::object_t>();
            object != nullptr && object->contains("viewports")) {
        const auto& frozen = object->at("viewports").get<JsonValue::array_t>();
        REQUIRE(viewports.size() == frozen.size());
        for (std::size_t index = 0; index < viewports.size(); ++index) {
            INFO("viewport " << index);
            CHECK(viewports[index] == copied_texts(frozen[index]));
        }
    }
    REQUIRE(tui.stop());
}

void replay_mouse_routing_scenario(std::string_view name) {
    const auto bundle = selection_bundle();
    REQUIRE(bundle);
    const auto& scenario = mouse_scenario_of(*bundle, name);
    const auto& dimensions = scenario.at("dimensions");
    const auto& inputs = scenario.at("inputs");
    const auto& expected = scenario.at("expected");
    cch::tui::VirtualTerminal terminal({
            .columns = static_cast<std::size_t>(dimensions.at("columns").get_number()),
            .rows = static_cast<std::size_t>(dimensions.at("rows").get_number()),
    });
    cch::tui::AltScreen tui(terminal);
    std::vector<std::string> opened;
    std::size_t paste_count = 0;
    std::chrono::milliseconds now{0};
    tui.set_selection_click_clock([&now] { return now; });
    tui.set_link_open_sink([&opened](std::string_view url) noexcept { opened.emplace_back(url); });
    tui.set_right_click_paste_handler([&paste_count]() -> cch::support::ExpectedVoid {
        ++paste_count;
        return {};
    });
    const auto routed_text = name == "osc8-click-and-drag-routing"
                                     ? cch::tui::hyperlink("alpha", inputs.at("url").get_string()) + " plain"
                                     : "alpha beta";
    REQUIRE(tui.add_child(std::make_unique<cch::tui::Text>(routed_text, 0, 0)));
    REQUIRE(tui.start());
    REQUIRE(tui.render());

    if (name == "osc8-click-and-drag-routing") {
        const auto url = inputs.at("url").get_string();
        const auto text = inputs.at("text").get_string();
        REQUIRE(cch::tui::osc8_link_at_column(text, 1) == std::nullopt);
        REQUIRE(cch::tui::osc8_link_at_column(routed_text, 1) == url);
        REQUIRE(terminal.inject_input("\x1b[<0;2;1M"));
        REQUIRE(terminal.inject_input("\x1b[<0;2;1m"));
        CHECK(opened == copied_texts(expected.at("afterClick")));
        now += std::chrono::milliseconds{150};
        REQUIRE(terminal.inject_input("\x1b[<0;2;1M"));
        REQUIRE(terminal.inject_input("\x1b[<0;2;1m"));
        CHECK(opened == copied_texts(expected.at("afterWordClick")));
        now += std::chrono::milliseconds{150};
        REQUIRE(terminal.inject_input("\x1b[<0;2;1M"));
        REQUIRE(terminal.inject_input("\x1b[<32;5;1M"));
        REQUIRE(terminal.inject_input("\x1b[<0;5;1m"));
        CHECK(opened == copied_texts(expected.at("afterDrag")));
    } else {
        CHECK(expected.at("afterPress").get_number() == 0);
        REQUIRE(inputs.at("platform").get_string() == "linux");
        const auto& press = inputs.at("press");
        CHECK(press.at("button").get_number() == 2);
        REQUIRE(terminal.inject_input("\x1b[<" + std::to_string(static_cast<int>(press.at("button").get_number())) +
                                      ";" + std::to_string(static_cast<int>(press.at("x").get_number())) + ";" +
                                      std::to_string(static_cast<int>(press.at("y").get_number())) + "M"));
        CHECK(paste_count == 1);
        CHECK(expected.at("afterPress").get_number() == 0);
        const auto release_button = static_cast<int>(inputs.at("releaseButton").get_number());
        REQUIRE(terminal.inject_input("\x1b[<" + std::to_string(release_button) + ";" +
                                      std::to_string(static_cast<int>(press.at("x").get_number())) + ";" +
                                      std::to_string(static_cast<int>(press.at("y").get_number())) + "m"));
        const auto motion_button = static_cast<int>(inputs.at("motionButton").get_number());
        REQUIRE(terminal.inject_input("\x1b[<" + std::to_string(motion_button) + ";" +
                                      std::to_string(static_cast<int>(press.at("x").get_number())) + ";" +
                                      std::to_string(static_cast<int>(press.at("y").get_number())) + "M"));
        CHECK(paste_count == 1);
        CHECK(expected.at("afterMotion").get_number() == 0);
        CHECK_FALSE(tui.has_active_selection());
    }
    REQUIRE(tui.stop());
}

} // namespace

TEST_CASE("AltScreen OSC 8 clicks and drags replay named frozen observations",
        "[tui][alt-screen][mouse][issue1009][compat-pi]") {
    replay_mouse_routing_scenario("osc8-click-and-drag-routing");
}

TEST_CASE("AltScreen secondary button replays named Linux paste routing observation",
        "[tui][alt-screen][mouse][issue1009][compat-pi]") {
    replay_mouse_routing_scenario("linux-secondary-button-paste");
}

TEST_CASE("AltScreen selection replays styled CJK cells across wrapped rows and copies frozen text",
        "[tui][alt-screen][selection][issue1006][compat-pi]") {
    const auto bundle = selection_bundle();
    REQUIRE(bundle);
    const auto& scenario = scenario_of(*bundle, "styled-cjk-wrapped-selection");
    const auto& dimensions = scenario.at("dimensions");
    const auto& inputs = scenario.at("inputs");
    const auto& expected = scenario.at("expected");
    cch::tui::VirtualTerminal terminal({
            .columns = static_cast<std::size_t>(dimensions.at("columns").get_number()),
            .rows = static_cast<std::size_t>(dimensions.at("rows").get_number()),
    });
    cch::tui::AltScreen tui(terminal);
    std::vector<std::string> copied;
    tui.set_copy_on_select(false);
    tui.set_selection_copy_sink([&copied](std::string_view text) -> cch::support::ExpectedVoid {
        copied.emplace_back(text);
        return {};
    });
    REQUIRE(tui.add_child(std::make_unique<cch::tui::Text>(inputs.at("text").get_string(), 0, 0)));
    REQUIRE(tui.start());
    drag(terminal, inputs);
    REQUIRE(tui.render());

    CHECK(tui.has_active_selection() == expected.at("hasSelection").get_boolean());
    REQUIRE(terminal.cells().size() >= 2);
    const auto& expected_cells = expected.at("styledCells").get_array();
    REQUIRE(expected_cells.size() == 2);
    for (std::size_t row = 0; row < 2; ++row) {
        const auto& expected_row = expected_cells[row].get_array();
        REQUIRE(terminal.cells()[row].size() >= expected_row.size());
        for (std::size_t column = 0; column < expected_row.size(); ++column) {
            const auto& frozen = expected_row[column];
            const auto& actual = terminal.cells()[row][column];
            CHECK(actual.style.inverse == frozen.at("inverse").get_boolean());
            const auto width = static_cast<std::size_t>(frozen.at("width").get_number());
            if (width == 0) {
                CHECK(actual.continuation);
            } else {
                CHECK_FALSE(actual.continuation);
                CHECK(actual.grapheme == frozen.at("text").get_string());
            }
            const auto foreground = frozen.at("foreground").get_number();
            CHECK(actual.style.fg_color == (foreground < 0 ? "" : "31"));
        }
    }

    const auto copied_result = tui.copy_active_selection_to_clipboard();
    REQUIRE(copied_result);
    CHECK(*copied_result == expected.at("copyResult").get_boolean());
    CHECK(copied == copied_texts(expected.at("copied")));
    CHECK(tui.copy_on_select() == expected.at("copyOnSelect").get_boolean());
    REQUIRE(tui.stop());
}

TEST_CASE("AltScreen selection preserves the cell range through content and focus changes",
        "[tui][alt-screen][selection][issue1006][compat-pi]") {
    const auto bundle = selection_bundle();
    REQUIRE(bundle);
    const auto& scenario = scenario_of(*bundle, "selection-content-focus-and-clear-lifecycle");
    const auto& dimensions = scenario.at("dimensions");
    const auto& inputs = scenario.at("inputs");
    const auto& expected = scenario.at("expected");
    cch::tui::VirtualTerminal terminal({
            .columns = static_cast<std::size_t>(dimensions.at("columns").get_number()),
            .rows = static_cast<std::size_t>(dimensions.at("rows").get_number()),
    });
    cch::tui::AltScreen tui(terminal);
    std::vector<std::string> copied;
    tui.set_copy_on_select(false);
    tui.set_selection_copy_sink([&copied](std::string_view text) -> cch::support::ExpectedVoid {
        copied.emplace_back(text);
        return {};
    });
    auto text = std::make_unique<cch::tui::Text>(inputs.at("initialText").get_string(), 0, 0);
    auto* text_ptr = text.get();
    REQUIRE(tui.add_child(std::move(text)));
    REQUIRE(tui.start());

    const auto empty = tui.copy_active_selection_to_clipboard();
    REQUIRE(empty);
    CHECK_FALSE(*empty);
    drag(terminal, inputs);
    REQUIRE(tui.render());
    CHECK(tui.has_active_selection() == expected.at("selected").get_boolean());
    const auto first_copy = tui.copy_active_selection_to_clipboard();
    REQUIRE(first_copy);
    CHECK(*first_copy == expected.at("firstCopyResult").get_boolean());

    text_ptr->set_text(inputs.at("updatedText").get_string());
    REQUIRE(tui.render());
    CHECK(tui.has_active_selection() == expected.at("afterContentChange").get_boolean());
    const auto updated_copy = tui.copy_active_selection_to_clipboard();
    REQUIRE(updated_copy);
    CHECK(*updated_copy == expected.at("updatedCopyResult").get_boolean());

    REQUIRE(terminal.inject_input("\x1b[O"));
    REQUIRE(terminal.inject_input("\x1b[I"));
    CHECK(tui.has_active_selection() == expected.at("afterFocusChange").get_boolean());

    const auto& click = inputs.at("contentClick");
    const auto x = static_cast<std::size_t>(click.at("x").get_number());
    const auto y = static_cast<std::size_t>(click.at("y").get_number());
    REQUIRE(terminal.inject_input("\x1b[<0;" + std::to_string(x) + ";" + std::to_string(y) + "M"));
    REQUIRE(terminal.inject_input("\x1b[<0;" + std::to_string(x) + ";" + std::to_string(y) + "m"));
    REQUIRE(tui.render());
    CHECK(tui.has_active_selection() == expected.at("afterContentClick").get_boolean());
    CHECK(copied == copied_texts(expected.at("copied")));
    REQUIRE(tui.stop());
}

TEST_CASE("AltScreen selection copy returns the frozen specific clipboard failure",
        "[tui][alt-screen][selection][issue1006][compat-pi]") {
    const auto bundle = selection_bundle();
    REQUIRE(bundle);
    const auto& scenario = scenario_of(*bundle, "selection-copy-specific-failure");
    const auto& dimensions = scenario.at("dimensions");
    const auto& inputs = scenario.at("inputs");
    const auto& expected = scenario.at("expected");
    cch::tui::VirtualTerminal terminal({
            .columns = static_cast<std::size_t>(dimensions.at("columns").get_number()),
            .rows = static_cast<std::size_t>(dimensions.at("rows").get_number()),
    });
    cch::tui::AltScreen tui(terminal);
    std::vector<std::string> attempted;
    tui.set_copy_on_select(false);
    tui.set_selection_copy_sink([&attempted, &expected](std::string_view text) -> cch::support::ExpectedVoid {
        attempted.emplace_back(text);
        return std::unexpected(
                cch::support::make_error(cch::support::ErrorCode::Unknown, expected.at("copyError").get_string()));
    });
    REQUIRE(tui.add_child(std::make_unique<cch::tui::Text>(inputs.at("text").get_string(), 0, 0)));
    REQUIRE(tui.start());
    // The selection source lines fill during a rendered frame; drag, then
    // render before the copy attempt (the frozen drag alone leaves no rows).
    drag(terminal, inputs);
    REQUIRE(tui.render());
    const auto result = tui.copy_active_selection_to_clipboard();
    REQUIRE_FALSE(result);
    CHECK(result.error().message == expected.at("copyError").get_string());
    CHECK(attempted == copied_texts(expected.at("copied")));
    REQUIRE(tui.stop());
}

TEST_CASE("AltScreen copy-on-select uses the frozen default and ignores an empty click",
        "[tui][alt-screen][selection][issue1006][compat-pi]") {
    const auto bundle = selection_bundle();
    REQUIRE(bundle);
    const auto& scenario = scenario_of(*bundle, "copy-on-select-default-and-empty-range");
    const auto& dimensions = scenario.at("dimensions");
    const auto& inputs = scenario.at("inputs");
    const auto& expected = scenario.at("expected");
    cch::tui::VirtualTerminal terminal({
            .columns = static_cast<std::size_t>(dimensions.at("columns").get_number()),
            .rows = static_cast<std::size_t>(dimensions.at("rows").get_number()),
    });
    cch::tui::AltScreen tui(terminal);
    std::vector<std::string> copied;
    tui.set_selection_copy_sink([&copied](std::string_view text) -> cch::support::ExpectedVoid {
        copied.emplace_back(text);
        return {};
    });
    REQUIRE(tui.add_child(std::make_unique<cch::tui::Text>(inputs.at("text").get_string(), 0, 0)));
    REQUIRE(tui.start());
    CHECK(tui.copy_on_select() == expected.at("defaultCopyOnSelect").get_boolean());

    const auto& empty_click = inputs.at("emptyClick");
    const auto x = static_cast<std::size_t>(empty_click.at("x").get_number());
    const auto y = static_cast<std::size_t>(empty_click.at("y").get_number());
    REQUIRE(terminal.inject_input("\x1b[<0;" + std::to_string(x) + ";" + std::to_string(y) + "M"));
    REQUIRE(terminal.inject_input("\x1b[<0;" + std::to_string(x) + ";" + std::to_string(y) + "m"));
    CHECK(copied == copied_texts(expected.at("afterEmptyClick")));

    // Render before the drag so the selected rows exist when the gesture
    // completes and copy-on-select fires.
    REQUIRE(tui.render());
    drag(terminal, inputs);
    CHECK(tui.has_active_selection() == expected.at("hasSelection").get_boolean());
    CHECK(copied == copied_texts(expected.at("copied")));
    REQUIRE(tui.stop());
}

TEST_CASE("AltScreen multi-click selection replays the frozen word-line cycle",
        "[tui][alt-screen][selection][issue1007][compat-pi]") {
    replay_multiclick_scenario("multiclick-word-line-cycle");
}

TEST_CASE("AltScreen multi-click selection replays the frozen word classes and joiners",
        "[tui][alt-screen][selection][issue1007][compat-pi]") {
    replay_multiclick_scenario("multiclick-word-joiners-and-classes");
}

TEST_CASE("AltScreen multi-click selection replays the frozen word drag expansion",
        "[tui][alt-screen][selection][issue1007][compat-pi]") {
    replay_multiclick_scenario("multiclick-word-drag-expansion");
}

TEST_CASE("AltScreen multi-click selection replays the frozen wrapped-row line granularity",
        "[tui][alt-screen][selection][issue1007][compat-pi]") {
    replay_multiclick_scenario("multiclick-line-wrapped-rows");
}

TEST_CASE("AltScreen multi-click selection replays the frozen interval expiry and word identity",
        "[tui][alt-screen][selection][issue1007][compat-pi]") {
    replay_multiclick_scenario("multiclick-expiry-and-word-identity");
}

TEST_CASE("AltScreen multi-click selection replays the frozen focus-out cancellation",
        "[tui][alt-screen][selection][issue1007][compat-pi]") {
    replay_multiclick_scenario("multiclick-focus-out-cancels");
}

TEST_CASE("AltScreen multi-click selection replays the frozen scrollbar cancellation",
        "[tui][alt-screen][selection][issue1007][compat-pi]") {
    replay_multiclick_scenario("multiclick-scrollbar-cancels");
}

TEST_CASE("AltScreen multi-click selection replays the frozen dictionary-script word runs",
        "[tui][alt-screen][selection][issue1007][compat-pi]") {
    replay_multiclick_scenario("multiclick-dictionary-script-runs");
}

// The inclusive 500 ms / 501 ms interval boundary is pinned by the frozen
// `multiclick-expiry-and-word-identity` scenario steps and the AltScreen unit
// test on the same boundary.

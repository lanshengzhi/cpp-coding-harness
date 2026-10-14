// Named pi-v1.0.4 acceptance for fullscreen text selection (#1006). Every
// expectation comes from the verified frozen bundle; the cases exercise the
// public AltScreen, Text and VirtualTerminal seams.

#include <cch/tui/AltScreen.hpp>
#include <cch/tui/Text.hpp>
#include <cch/tui/VirtualTerminal.hpp>

#include "support/PiTuiEvidence.hpp"

#include <catch2/catch_test_macros.hpp>

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

[[nodiscard]] std::vector<std::string> copied_texts(const JsonValue& values) {
    std::vector<std::string> texts;
    for (const auto& value : values.get_array()) texts.push_back(value.get_string());
    return texts;
}

} // namespace

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
        return std::unexpected(cch::support::make_error(
                cch::support::ErrorCode::Unknown, expected.at("copyError").get_string()));
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

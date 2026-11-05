#include <cch/tui/Overlay.hpp>
#include <cch/tui/SelectList.hpp>
#include <cch/tui/Tui.hpp>
#include <cch/tui/VirtualTerminal.hpp>
#include <cch/tui/Utils.hpp>

#include "support/PiTuiEvidence.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

std::vector<cch::tui::SelectItem> make_items(std::size_t count) {
    std::vector<cch::tui::SelectItem> items;
    for (std::size_t index = 0; index < count; ++index) {
        items.push_back(cch::tui::SelectItem{
            .value = "item" + std::to_string(index),
            .label = "Item " + std::to_string(index),
            .description = "description " + std::to_string(index),
        });
    }
    return items;
}

} // namespace

TEST_CASE(
        "SelectList renders empty and unmatched filters through VirtualTerminal", "[tui][select-list][issue52][spec]") {
    cch::tui::VirtualTerminal terminal({.columns = 30, .rows = 3});
    cch::tui::Tui tui(terminal);
    auto list = std::make_unique<cch::tui::SelectList>(std::vector<cch::tui::SelectItem>{});
    auto* list_ptr = list.get();
    REQUIRE(tui.add_child(std::move(list)));
    REQUIRE(tui.start());
    REQUIRE(tui.set_focus(list_ptr));
    REQUIRE(tui.render());
    CHECK(terminal.screen()[0].find("No matching commands") != std::string::npos);

    list_ptr->set_items({});
    REQUIRE(tui.render());
    CHECK(terminal.screen()[0].find("No matching commands") != std::string::npos);
}

TEST_CASE("SelectList pages large bounded lists and adapts descriptions after resize",
        "[tui][select-list][issue52][spec]") {
    cch::tui::VirtualTerminal terminal({.columns = 60, .rows = 5});
    cch::tui::Tui tui(terminal);
    auto list = std::make_unique<cch::tui::SelectList>(
        make_items(12),
        cch::tui::SelectListOptions{.max_visible = 3});
    auto* list_ptr = list.get();
    REQUIRE(tui.add_child(std::move(list)));
    REQUIRE(tui.start());
    REQUIRE(tui.set_focus(list_ptr));
    REQUIRE(tui.render());
    REQUIRE(list_ptr->selected_item());
    CHECK(list_ptr->selected_item()->value == "item0");
    CHECK(terminal.screen()[0].find("description 0") != std::string::npos);

    for (int index = 0; index < 3; ++index)
        REQUIRE(terminal.inject_input("\x1b[B"));
    REQUIRE(tui.render());
    REQUIRE(list_ptr->selected_item());
    CHECK(list_ptr->selected_item()->value == "item3");
    CHECK(terminal.screen()[3].find("(4/12)") != std::string::npos);

    REQUIRE(terminal.inject_resize({.columns = 30, .rows = 4}));
    REQUIRE(tui.render());
    for (const auto& row : terminal.screen()) {
        CHECK(row.find("description") == std::string::npos);
    }
}

TEST_CASE("SelectList filters navigates wraps selects and cancels with semantic keys",
        "[tui][select-list][issue52][spec]") {
    std::string selected;
    std::string changed;
    std::size_t cancellations = 0;
    cch::tui::SelectList list(
        {
            {.value = "alpha", .label = "Alpha"},
            {.value = "alpine", .label = "Alpine"},
            {.value = "beta", .label = "Beta"},
        },
        cch::tui::SelectListOptions{
            .max_visible = 2,
            .on_select = [&selected](const cch::tui::SelectItem& item) -> cch::support::ExpectedVoid {
                selected = item.value;
                return {};
            },
            .on_cancel = [&cancellations]() -> cch::support::ExpectedVoid {
                ++cancellations;
                return {};
            },
            .on_selection_change = [&changed](const cch::tui::SelectItem& item) -> cch::support::ExpectedVoid {
                changed = item.value;
                return {};
            },
        });

    static_cast<void>(list.handle_input(cch::tui::KeyEvent{.key = "up"}));
    REQUIRE(list.selected_item());
    CHECK(list.selected_item()->value == "beta");
    CHECK(changed == "beta");
    static_cast<void>(list.handle_input(cch::tui::KeyEvent{.key = "down"}));
    static_cast<void>(list.handle_input(cch::tui::KeyEvent{.key = "down"}));
    REQUIRE(list.selected_item());
    CHECK(list.selected_item()->value == "alpine");
    static_cast<void>(list.handle_input(cch::tui::KeyEvent{.key = "enter"}));
    CHECK(selected == "alpine");
    static_cast<void>(list.handle_input(cch::tui::KeyEvent{.key = "escape"}));
    CHECK(cancellations == 1);
}

TEST_CASE("SelectList normalizes and aligns descriptions within configured columns",
        "[tui][select-list][issue52][spec]") {
    cch::tui::SelectList list(
        {
            {.value = "short", .label = "Short", .description = "line one\nline two"},
            {
                .value = "long",
                .label = "A very long primary label",
                .description = "second description",
            },
        },
        cch::tui::SelectListOptions{
            .layout = {
                .min_primary_column_width = 12,
                .max_primary_column_width = 20,
            },
        });

    const auto rendered = list.render(80);
    REQUIRE(rendered);
    REQUIRE(rendered->lines.size() == 2);
    CHECK(rendered->lines[0].find('\n') == std::string::npos);
    CHECK(rendered->lines[0].find("line one line two") != std::string::npos);
    const auto first_description = rendered->lines[0].find("line one");
    const auto second_description = rendered->lines[1].find("second description");
    REQUIRE(first_description != std::string::npos);
    REQUIRE(second_description != std::string::npos);
    CHECK(cch::tui::visible_width(rendered->lines[0].substr(0, first_description)) ==
          cch::tui::visible_width(rendered->lines[1].substr(0, second_description)));
}

TEST_CASE("SelectList dispatches configured keys from its effective registry", "[tui][select-list][issue57][spec]") {
    cch::tui::KeybindingResolutionRequest request;
    request.definitions = cch::tui::builtin_tui_keybinding_definitions();
    request.overrides = {{.id = "tui.select.confirm", .keys = {"f2"}}};
    const auto keybindings = cch::tui::resolve_keybindings(std::move(request));
    REQUIRE(keybindings);

    std::size_t selections = 0;
    cch::tui::SelectList list(
        make_items(1),
        cch::tui::SelectListOptions{
            .on_select = [&selections](const cch::tui::SelectItem&) -> cch::support::ExpectedVoid {
                ++selections;
                return {};
            },
            .keybindings = keybindings->registry,
        });
    static_cast<void>(list.handle_input(cch::tui::KeyEvent{.key = "enter"}));
    CHECK(selections == 0);
    static_cast<void>(list.handle_input(cch::tui::KeyEvent{.key = "f2"}));
    CHECK(selections == 1);
}

TEST_CASE("SelectList resolves a key shared by two actions in dispatch order", "[tui][select-list][spec]") {
    // f9 claims up and confirm; selection movement leads pi's select-list chain.
    auto registry = std::make_shared<const cch::tui::KeybindingRegistry>(std::vector<cch::tui::EffectiveKeybinding>{
            {.id = "tui.select.up", .keys = {"f9"}},
            {.id = "tui.select.confirm", .keys = {"f9", "enter"}},
    });
    std::size_t selections = 0;
    cch::tui::SelectList list(make_items(3),
            cch::tui::SelectListOptions{
                    .on_select = [&selections](const cch::tui::SelectItem&) -> cch::support::ExpectedVoid {
                        ++selections;
                        return {};
                    },
                    .keybindings = std::move(registry),
            });
    static_cast<void>(list.handle_input(cch::tui::KeyEvent{.key = "f9"}));
    CHECK(selections == 0);
    REQUIRE(list.selected_item());
    CHECK(list.selected_item()->value == "item2"); // up wraps to the tail
    static_cast<void>(list.handle_input(cch::tui::KeyEvent{.key = "enter"}));
    CHECK(selections == 1);
}

TEST_CASE("SelectList renders descriptions in an overlay with bounded rows",
        "[tui][select-list][overlay][issue52][spec]") {
    cch::tui::VirtualTerminal terminal({.columns = 70, .rows = 6});
    cch::tui::Tui tui(terminal);
    cch::tui::OverlayOptions overlay_options;
    overlay_options.max_height = 3.0;
    auto overlay = std::make_unique<cch::tui::Overlay>(std::move(overlay_options));
    auto list = std::make_unique<cch::tui::SelectList>(make_items(8), cch::tui::SelectListOptions{.max_visible = 2});
    REQUIRE(overlay->add_child(std::move(list)));
    REQUIRE(overlay->focus_first());
    auto* overlay_ptr = overlay.get();
    REQUIRE(tui.add_overlay(std::move(overlay)));
    REQUIRE(tui.start());
    REQUIRE(tui.set_focus(overlay_ptr));
    REQUIRE(tui.render());

    CHECK(terminal.screen()[1].find("Item 0") != std::string::npos);
    CHECK(terminal.screen()[1].find("description 0") != std::string::npos);
    CHECK(terminal.screen()[3].find("(1/8)") != std::string::npos);
}

TEST_CASE("SelectList centers its default five item window and marks the selection", "[tui][select-list][issue967]") {
    auto items = make_items(9);
    for (auto& item : items)
        item.description.reset();
    cch::tui::SelectList list(std::move(items));
    list.set_selected_index(6);

    const auto rendered = list.render(60);
    REQUIRE(rendered);
    REQUIRE(rendered->lines.size() == 6);
    CHECK(rendered->lines[0].find("Item 4") != std::string::npos);
    CHECK(rendered->lines[1].find("Item 5") != std::string::npos);
    CHECK(rendered->lines[2].find("→ Item 6") != std::string::npos);
    CHECK(rendered->lines[4].find("Item 8") != std::string::npos);
    CHECK(rendered->lines[5].find("(7/9)") != std::string::npos);
}

TEST_CASE("SelectList truncates narrow rows and hides descriptions", "[tui][select-list][issue967]") {
    cch::tui::SelectList list({{.value = "long-value", .label = "Long primary label", .description = "details"}});

    const auto rendered = list.render(10);
    REQUIRE(rendered);
    REQUIRE(rendered->lines.size() == 1);
    CHECK(cch::tui::visible_width(rendered->lines[0]) <= 10);
    CHECK(rendered->lines[0].find("details") == std::string::npos);
    CHECK(rendered->lines[0].starts_with("→ "));
}

TEST_CASE("SelectList only shows a scroll indicator when the visible window clips", "[tui][select-list][issue967]") {
    auto items = make_items(7);
    for (auto& item : items)
        item.description.reset();
    cch::tui::SelectList list(std::move(items));
    const auto top = list.render(60);
    REQUIRE(top);
    REQUIRE(top->lines.size() == 6);
    CHECK(top->lines.back().find("(1/7)") != std::string::npos);

    auto full_items = make_items(5);
    for (auto& item : full_items)
        item.description.reset();
    list.set_items(std::move(full_items));
    const auto full = list.render(60);
    REQUIRE(full);
    REQUIRE(full->lines.size() == 5);
    CHECK(full->lines.back().find("(") == std::string::npos);
}

TEST_CASE("SelectList bubbles unhandled keys and replaces dynamic item sets", "[tui][select-list][issue967]") {
    cch::tui::SelectList list(make_items(3));
    CHECK(list.handle_input(cch::tui::KeyEvent{.key = "x"}) == cch::tui::InputAdmissionOutcome::Unhandled);
    CHECK(list.handle_input(cch::tui::KeyEvent{.key = "pageDown"}) == cch::tui::InputAdmissionOutcome::Unhandled);
    CHECK(list.handle_input(cch::tui::KeyEvent{.key = "down", .type = cch::tui::KeyEventType::Release}) ==
            cch::tui::InputAdmissionOutcome::Unhandled);

    list.set_selected_index(2);
    auto replacement = make_items(4);
    replacement[1].value = "item2";
    replacement[1].label = "Updated selected item";
    list.set_items(std::move(replacement));
    REQUIRE(list.selected_item());
    CHECK(list.selected_item()->value == "item2");
    CHECK(list.selected_item()->label == "Updated selected item");

    list.set_items({{.value = "replacement", .label = "Replacement"}});
    REQUIRE(list.selected_item());
    CHECK(list.selected_item()->value == "replacement");
    list.set_items({});
    CHECK_FALSE(list.selected_item());
    const auto empty = list.render(60);
    REQUIRE(empty);
    REQUIRE(empty->lines.size() == 1);
    CHECK(empty->lines[0] == "  No matching commands");
}

namespace {

[[nodiscard]] const cch::support::JsonValue* select_list_scenario(
        const cch::support::JsonValue& fixture, std::string_view name) {
    for (const auto& scenario : fixture.at("scenarios").get_array()) {
        if (scenario.at("name").get_string() == name) return &scenario;
    }
    return nullptr;
}

[[nodiscard]] std::vector<std::string> string_array(const cch::support::JsonValue& value) {
    std::vector<std::string> result;
    for (const auto& entry : value.get_array())
        result.push_back(entry.get_string());
    return result;
}

[[nodiscard]] std::vector<cch::tui::SelectItem> named_items(const cch::support::JsonValue& values) {
    std::vector<cch::tui::SelectItem> items;
    for (const auto& item : values.get_array()) {
        const auto name = item.get_string();
        std::string label;
        label.reserve(name.size());
        bool capitalize_next = true;
        for (const char character : name) {
            if (character == '-') {
                label.push_back(' ');
                capitalize_next = true;
            } else {
                label.push_back(capitalize_next && character >= 'a' && character <= 'z'
                                        ? static_cast<char>(character - ('a' - 'A'))
                                        : character);
                capitalize_next = false;
            }
        }
        items.push_back({.value = name, .label = std::move(label)});
    }
    return items;
}

[[nodiscard]] std::vector<cch::tui::SelectItem> select_items(const cch::support::JsonValue& values) {
    std::vector<cch::tui::SelectItem> items;
    for (const auto& item : values.get_array()) {
        cch::tui::SelectItem value{
                .value = item.at("value").get_string(),
                .label = item.at("label").get_string(),
        };
        if (const auto description = item.get_object().find("description");
                description != item.get_object().end() && description->second.holds<std::string>()) {
            value.description = description->second.get_string();
        }
        items.push_back(std::move(value));
    }
    return items;
}

} // namespace

TEST_CASE("SelectList matches frozen pi cycling and exact selection callback order",
        "[tui][select-list][issue967][compat-pi]") {
    const auto fixture = cch::tests::read_pi_tui_evidence("screen-state.json");
    REQUIRE(fixture);
    const auto* scenario = select_list_scenario(*fixture, "select-list-cycling-and-selection");
    REQUIRE(scenario != nullptr);
    const auto& expected = scenario->at("expected");
    const auto inputs = named_items(scenario->at("inputs").at("items"));
    std::vector<std::string> callback_events;
    cch::tui::SelectList list(inputs,
            cch::tui::SelectListOptions{
                    .on_select = [&callback_events](const auto& item) -> cch::support::ExpectedVoid {
                        callback_events.push_back("select:" + item.value);
                        return {};
                    },
                    .on_selection_change = [&callback_events](const auto& item) -> cch::support::ExpectedVoid {
                        callback_events.push_back("change:" + item.value);
                        return {};
                    },
            });
    const auto check_observation = [&list](const cch::support::JsonValue& recorded) {
        const auto rendered = list.render(24);
        REQUIRE(rendered);
        CHECK(list.selected_item()->value == recorded.at("selected").get_string());
        CHECK(rendered->lines == string_array(recorded.at("rows")));
        std::vector<std::size_t> widths;
        for (const auto& line : rendered->lines)
            widths.push_back(cch::tui::visible_width(line));
        std::vector<std::size_t> expected_widths;
        for (const auto& width : recorded.at("visibleWidths").get_array())
            expected_widths.push_back(static_cast<std::size_t>(width.get_number()));
        CHECK(widths == expected_widths);
    };
    check_observation(expected.at("initial"));
    static_cast<void>(list.handle_input(cch::tui::KeyEvent{.key = "up"}));
    check_observation(expected.at("up"));
    static_cast<void>(list.handle_input(cch::tui::KeyEvent{.key = "down"}));
    static_cast<void>(list.handle_input(cch::tui::KeyEvent{.key = "down"}));
    check_observation(expected.at("wrapped"));
    static_cast<void>(list.handle_input(cch::tui::KeyEvent{.key = "enter"}));
    CHECK(callback_events == string_array(expected.at("callbackEvents")));
}

TEST_CASE("SelectList matches frozen pi centered five-row windows and scroll counters",
        "[tui][select-list][issue967][compat-pi]") {
    const auto fixture = cch::tests::read_pi_tui_evidence("screen-state.json");
    REQUIRE(fixture);
    const auto* scenario = select_list_scenario(*fixture, "select-list-centered-five-row-window");
    REQUIRE(scenario != nullptr);
    const auto& expected = scenario->at("expected");
    std::vector<cch::tui::SelectItem> items;
    for (std::size_t index = 1; index <= 9; ++index) {
        const auto name = "item-" + std::to_string(index);
        const auto label = "Item " + std::to_string(index);
        items.push_back({.value = name, .label = label});
    }
    cch::tui::SelectList list(std::move(items));
    const auto check_observation = [&list](const cch::support::JsonValue& recorded) {
        const auto rendered = list.render(24);
        REQUIRE(rendered);
        REQUIRE(list.selected_item());
        CHECK(list.selected_item()->value == recorded.at("selected").get_string());
        CHECK(rendered->lines == string_array(recorded.at("rows")));
        std::vector<std::size_t> widths;
        for (const auto& line : rendered->lines)
            widths.push_back(cch::tui::visible_width(line));
        std::vector<std::size_t> expected_widths;
        for (const auto& width : recorded.at("visibleWidths").get_array())
            expected_widths.push_back(static_cast<std::size_t>(width.get_number()));
        CHECK(widths == expected_widths);
    };
    check_observation(expected.at("initial"));
    for (std::size_t index = 0; index < 6; ++index)
        static_cast<void>(list.handle_input(cch::tui::KeyEvent{.key = "down"}));
    check_observation(expected.at("middle"));
    for (std::size_t index = 0; index < 2; ++index)
        static_cast<void>(list.handle_input(cch::tui::KeyEvent{.key = "down"}));
    check_observation(expected.at("end"));
}

TEST_CASE("SelectList matches frozen pi value-prefix filtering and selection reset",
        "[tui][select-list][issue967][compat-pi]") {
    const auto fixture = cch::tests::read_pi_tui_evidence("screen-state.json");
    REQUIRE(fixture);
    const auto* scenario = select_list_scenario(*fixture, "select-list-value-prefix-filter-and-selection-reset");
    REQUIRE(scenario != nullptr);
    const auto items = select_items(scenario->at("inputs").at("items"));
    const auto& operations = scenario->at("inputs").at("operations").get_array();
    const auto& observations = scenario->at("expected").at("observations").get_array();
    REQUIRE(operations.size() == observations.size());
    std::vector<std::string> callback_events;
    cch::tui::SelectList list(items,
            cch::tui::SelectListOptions{
                    .on_select = [&callback_events](const auto& item) -> cch::support::ExpectedVoid {
                        callback_events.push_back("select:" + item.value);
                        return {};
                    },
                    .on_selection_change = [&callback_events](const auto& item) -> cch::support::ExpectedVoid {
                        callback_events.push_back("change:" + item.value);
                        return {};
                    },
            });

    for (std::size_t index = 0; index < operations.size(); ++index) {
        const auto& operation = operations[index];
        if (operation.at("kind").get_string() == "select") {
            list.set_selected_index(static_cast<std::size_t>(operation.at("index").get_number()));
        } else {
            list.set_filter(operation.at("value").get_string());
        }
        const auto& recorded = observations[index];
        const auto rendered = list.render(60);
        REQUIRE(rendered);
        CHECK(rendered->lines == string_array(recorded.at("rows")));
        const auto filtered = list.filtered_items();
        std::vector<std::string> values;
        for (const auto& item : filtered)
            values.push_back(item.value);
        CHECK(values == string_array(recorded.at("filteredValues")));
        const auto selected = list.selected_item();
        if (recorded.at("selected").holds<std::nullptr_t>()) {
            CHECK_FALSE(selected.has_value());
        } else {
            REQUIRE(selected);
            CHECK(selected->value == recorded.at("selected").get_string());
        }
        std::vector<std::size_t> widths;
        for (const auto& line : rendered->lines)
            widths.push_back(cch::tui::visible_width(line));
        std::vector<std::size_t> expected_widths;
        for (const auto& width : recorded.at("visibleWidths").get_array())
            expected_widths.push_back(static_cast<std::size_t>(width.get_number()));
        CHECK(widths == expected_widths);
    }
    CHECK(callback_events == string_array(scenario->at("expected").at("callbackEvents")));

    list.set_filter("alp");
    list.set_selected_index(1);
    list.set_items({{.value = "alpine", .label = "Alpine refreshed"},
            {.value = "alphabet", .label = "Alphabet refreshed"},
            {.value = "beta", .label = "Filtered out"}});
    REQUIRE(list.selected_item());
    CHECK(list.selected_item()->value == "alphabet");
    CHECK((list.filtered_items() == std::vector<cch::tui::SelectItem>{{.value = "alpine", .label = "Alpine refreshed"},
                                            {.value = "alphabet", .label = "Alphabet refreshed"}}));
    auto rendered = list.render(60);
    REQUIRE(rendered);
    CHECK((rendered->lines == std::vector<std::string>{"  Alpine refreshed", "→ Alphabet refreshed"}));
    list.set_items({{.value = "alpha", .label = "Alpha replacement"}, {.value = "beta", .label = "Beta"}});
    REQUIRE(list.selected_item());
    CHECK(list.selected_item()->value == "alpha");
    CHECK((list.filtered_items() ==
            std::vector<cch::tui::SelectItem>{{.value = "alpha", .label = "Alpha replacement"}}));
    CHECK(callback_events.empty());
}

TEST_CASE("SelectList matches frozen pi Unicode description normalization", "[tui][select-list][issue967][compat-pi]") {
    const auto fixture = cch::tests::read_pi_tui_evidence("screen-state.json");
    REQUIRE(fixture);
    const auto* scenario = select_list_scenario(*fixture, "select-list-unicode-description-normalization");
    REQUIRE(scenario != nullptr);
    auto items = select_items(scenario->at("inputs").at("items"));
    cch::tui::SelectList list(std::move(items));
    const auto& expected = scenario->at("expected").at("observations").get_array();
    const auto& selected_indices = scenario->at("inputs").at("selectedIndices").get_array();
    REQUIRE(expected.size() == selected_indices.size());
    for (std::size_t index = 0; index < expected.size(); ++index) {
        list.set_selected_index(static_cast<std::size_t>(selected_indices[index].get_number()));
        const auto rendered = list.render(80);
        REQUIRE(rendered);
        CHECK(rendered->lines == string_array(expected[index].at("rows")));
        std::vector<std::size_t> widths;
        for (const auto& line : rendered->lines)
            widths.push_back(cch::tui::visible_width(line));
        std::vector<std::size_t> expected_widths;
        for (const auto& width : expected[index].at("visibleWidths").get_array())
            expected_widths.push_back(static_cast<std::size_t>(width.get_number()));
        CHECK(widths == expected_widths);
    }
}

TEST_CASE("SelectList matches frozen pi description allocation and wide glyph truncation",
        "[tui][select-list][issue967][compat-pi]") {
    const auto fixture = cch::tests::read_pi_tui_evidence("screen-state.json");
    REQUIRE(fixture);
    const auto* descriptions = select_list_scenario(*fixture, "select-list-description-width-boundaries");
    const auto* narrow = select_list_scenario(*fixture, "select-list-narrow-wide-glyph-truncation");
    REQUIRE(descriptions != nullptr);
    REQUIRE(narrow != nullptr);
    const auto& expected_widths = descriptions->at("expected").at("widths").get_array();
    cch::tui::SelectList description_list({
            {.value = "a", .label = "Alpha", .description = "First descriptive row"},
            {.value = "b", .label = "Beta", .description = "Second descriptive row"},
    });
    REQUIRE(expected_widths.size() == 3);
    for (const auto& expected : expected_widths) {
        const auto width = static_cast<std::size_t>(expected.at("width").get_number());
        const auto rendered = description_list.render(width);
        REQUIRE(rendered);
        CHECK(rendered->lines == string_array(expected.at("rows")));
        std::vector<std::size_t> visible_widths;
        for (const auto& line : rendered->lines)
            visible_widths.push_back(cch::tui::visible_width(line));
        std::vector<std::size_t> recorded_widths;
        for (const auto& entry : expected.at("visibleWidths").get_array())
            recorded_widths.push_back(static_cast<std::size_t>(entry.get_number()));
        CHECK(visible_widths == recorded_widths);
    }
    const auto& narrow_inputs = narrow->at("inputs");
    cch::tui::SelectList narrow_list({
            {.value = narrow_inputs.at("label").get_string(),
                    .label = narrow_inputs.at("label").get_string(),
                    .description = narrow_inputs.at("description").get_string()},
    });
    const auto narrow_rendered = narrow_list.render(static_cast<std::size_t>(narrow_inputs.at("width").get_number()));
    REQUIRE(narrow_rendered);
    CHECK(narrow_rendered->lines == string_array(narrow->at("expected").at("rows")));
    CHECK(cch::tui::visible_width(narrow_rendered->lines.front()) == 8);
    CHECK(narrow_rendered->lines.front().ends_with("\x1b[0m"));
}

TEST_CASE("SelectList selection callbacks own snapshots across reentrant set_items",
        "[tui][select-list][issue967][lifetime]") {
    cch::tui::SelectList* selection_list = nullptr;
    const std::string selection_value(64, 's');
    bool selection_change_snapshot_survived = false;
    cch::tui::SelectList selection_list_under_test(
            {{.value = "first", .label = "First"}, {.value = selection_value, .label = "Second"}},
            cch::tui::SelectListOptions{
                    .on_selection_change = [&](const cch::tui::SelectItem& item) -> cch::support::ExpectedVoid {
                        selection_list->set_items({{.value = "replacement", .label = "Replacement"}});
                        selection_change_snapshot_survived = item.value == selection_value;
                        return {};
                    },
            });
    selection_list = &selection_list_under_test;
    static_cast<void>(selection_list->handle_input(cch::tui::KeyEvent{.key = "down"}));
    CHECK(selection_change_snapshot_survived);

    cch::tui::SelectList* selected_list = nullptr;
    const std::string selected_value(64, 'c');
    bool select_snapshot_survived = false;
    cch::tui::SelectList selected_list_under_test({{.value = selected_value, .label = "Chosen"}},
            cch::tui::SelectListOptions{
                    .on_select = [&](const cch::tui::SelectItem& item) -> cch::support::ExpectedVoid {
                        selected_list->set_items({{.value = "replacement", .label = "Replacement"}});
                        select_snapshot_survived = item.value == selected_value;
                        return {};
                    },
            });
    selected_list = &selected_list_under_test;
    static_cast<void>(selected_list->handle_input(cch::tui::KeyEvent{.key = "enter"}));
    CHECK(select_snapshot_survived);
}

TEST_CASE("SelectList matches frozen pi unbound key non-mutation", "[tui][select-list][issue967][compat-pi]") {
    const auto fixture = cch::tests::read_pi_tui_evidence("screen-state.json");
    REQUIRE(fixture);
    const auto* scenario = select_list_scenario(*fixture, "select-list-unbound-key-forwarding");
    REQUIRE(scenario != nullptr);
    cch::tui::SelectList list({{.value = "first", .label = "First"}, {.value = "second", .label = "Second"}});
    for (const auto& observation : scenario->at("expected").at("observations").get_array()) {
        REQUIRE(list.selected_item());
        CHECK(list.selected_item()->value == observation.at("before").get_string());
        const auto& input = observation.at("input").get_string();
        const auto key = input == "\x1b[6~" ? "pageDown" : input.c_str();
        CHECK(list.handle_input(cch::tui::KeyEvent{.key = key}) == cch::tui::InputAdmissionOutcome::Unhandled);
        CHECK(list.selected_item()->value == observation.at("after").get_string());
    }
    REQUIRE(list.selected_item());
    CHECK(list.selected_item()->value ==
            scenario->at("expected").at("observations").get_array().back().at("after").get_string());
}

namespace {

[[nodiscard]] cch::support::Expected<cch::support::JsonValue> select_list_v130_scenario(std::string_view name) {
    auto fixture = cch::tests::read_pi_v130_audit_evidence("audit-1219", "select-list.json");
    if (!fixture) return std::unexpected(fixture.error());
    for (const auto& item : fixture->at("scenarios").get_array()) {
        if (item.at("name").get_string() == name) return cch::support::JsonValue{item};
    }
    return std::unexpected(
            cch::support::make_error(cch::support::ErrorCode::Validation, "Scenario not found: " + std::string(name)));
}

} // namespace

TEST_CASE("pi-v1.3.0 SelectList cycling and selection callback order matches frozen observation",
        "[tui][select-list][issue1219]") {
    const auto scenario = select_list_v130_scenario("select-list-cycling-and-selection");
    REQUIRE(scenario);
    const auto& expected = scenario->at("expected");
    const auto inputs = named_items(scenario->at("inputs").at("items"));
    std::vector<std::string> callback_events;
    cch::tui::SelectList list(inputs,
            cch::tui::SelectListOptions{
                    .on_select = [&callback_events](const auto& item) -> cch::support::ExpectedVoid {
                        callback_events.push_back("select:" + item.value);
                        return {};
                    },
                    .on_selection_change = [&callback_events](const auto& item) -> cch::support::ExpectedVoid {
                        callback_events.push_back("change:" + item.value);
                        return {};
                    },
            });
    const auto check_observation = [&list](const cch::support::JsonValue& recorded) {
        const auto rendered = list.render(24);
        REQUIRE(rendered);
        CHECK(list.selected_item()->value == recorded.at("selected").get_string());
        CHECK(rendered->lines == string_array(recorded.at("rows")));
        std::vector<std::size_t> widths;
        for (const auto& line : rendered->lines)
            widths.push_back(cch::tui::visible_width(line));
        std::vector<std::size_t> expected_widths;
        for (const auto& width : recorded.at("visibleWidths").get_array())
            expected_widths.push_back(static_cast<std::size_t>(width.get_number()));
        CHECK(widths == expected_widths);
    };
    check_observation(expected.at("initial"));
    static_cast<void>(list.handle_input(cch::tui::KeyEvent{.key = "up"}));
    check_observation(expected.at("up"));
    static_cast<void>(list.handle_input(cch::tui::KeyEvent{.key = "down"}));
    static_cast<void>(list.handle_input(cch::tui::KeyEvent{.key = "down"}));
    check_observation(expected.at("wrapped"));
    static_cast<void>(list.handle_input(cch::tui::KeyEvent{.key = "enter"}));
    CHECK(callback_events == string_array(expected.at("callbackEvents")));
}

TEST_CASE("pi-v1.3.0 SelectList centered five-row window and scroll counters match frozen observation",
        "[tui][select-list][issue1219]") {
    const auto scenario = select_list_v130_scenario("select-list-centered-five-row-window");
    REQUIRE(scenario);
    const auto& expected = scenario->at("expected");
    std::vector<cch::tui::SelectItem> items;
    for (std::size_t index = 1; index <= 9; ++index) {
        const auto name = "item-" + std::to_string(index);
        const auto label = "Item " + std::to_string(index);
        items.push_back({.value = name, .label = label});
    }
    cch::tui::SelectList list(std::move(items));
    const auto check_observation = [&list](const cch::support::JsonValue& recorded) {
        const auto rendered = list.render(24);
        REQUIRE(rendered);
        REQUIRE(list.selected_item());
        CHECK(list.selected_item()->value == recorded.at("selected").get_string());
        CHECK(rendered->lines == string_array(recorded.at("rows")));
        std::vector<std::size_t> widths;
        for (const auto& line : rendered->lines)
            widths.push_back(cch::tui::visible_width(line));
        std::vector<std::size_t> expected_widths;
        for (const auto& width : recorded.at("visibleWidths").get_array())
            expected_widths.push_back(static_cast<std::size_t>(width.get_number()));
        CHECK(widths == expected_widths);
    };
    check_observation(expected.at("initial"));
    for (std::size_t index = 0; index < 6; ++index)
        static_cast<void>(list.handle_input(cch::tui::KeyEvent{.key = "down"}));
    check_observation(expected.at("middle"));
    for (std::size_t index = 0; index < 2; ++index)
        static_cast<void>(list.handle_input(cch::tui::KeyEvent{.key = "down"}));
    check_observation(expected.at("end"));
}

TEST_CASE("pi-v1.3.0 SelectList description allocation and wide glyph truncation match frozen observation",
        "[tui][select-list][issue1219]") {
    const auto boundary_scenario = select_list_v130_scenario("select-list-description-width-boundaries");
    REQUIRE(boundary_scenario);
    std::vector<cch::tui::SelectItem> boundary_items = {
            {.value = "a", .label = "Alpha", .description = "First descriptive row"},
            {.value = "b", .label = "Beta", .description = "Second descriptive row"},
    };
    cch::tui::SelectList boundary_list(std::move(boundary_items));
    for (const auto& observation : boundary_scenario->at("expected").at("widths").get_array()) {
        const auto width = static_cast<std::size_t>(observation.at("width").get_number());
        const auto rendered = boundary_list.render(width);
        REQUIRE(rendered);
        CHECK(rendered->lines == string_array(observation.at("rows")));
        std::vector<std::size_t> widths;
        for (const auto& line : rendered->lines)
            widths.push_back(cch::tui::visible_width(line));
        std::vector<std::size_t> expected_widths;
        for (const auto& expected_width : observation.at("visibleWidths").get_array())
            expected_widths.push_back(static_cast<std::size_t>(expected_width.get_number()));
        CHECK(widths == expected_widths);
    }

    const auto narrow_scenario = select_list_v130_scenario("select-list-narrow-wide-glyph-truncation");
    REQUIRE(narrow_scenario);
    std::vector<cch::tui::SelectItem> narrow_items = {
            {.value = "wide", .label = "界界界界", .description = "hidden when narrow"},
    };
    cch::tui::SelectList narrow_list(std::move(narrow_items));
    const auto rendered = narrow_list.render(10);
    REQUIRE(rendered);
    CHECK(rendered->lines == string_array(narrow_scenario->at("expected").at("rows")));
    std::vector<std::size_t> widths;
    for (const auto& line : rendered->lines)
        widths.push_back(cch::tui::visible_width(line));
    std::vector<std::size_t> expected_widths;
    for (const auto& expected_width : narrow_scenario->at("expected").at("visibleWidths").get_array())
        expected_widths.push_back(static_cast<std::size_t>(expected_width.get_number()));
    CHECK(widths == expected_widths);
}

TEST_CASE("pi-v1.3.0 SelectList value-prefix filtering and selection reset match frozen observation",
        "[tui][select-list][issue1219]") {
    const auto scenario = select_list_v130_scenario("select-list-value-prefix-filter-and-selection-reset");
    REQUIRE(scenario);
    const auto items = select_items(scenario->at("inputs").at("items"));
    const auto& operations = scenario->at("inputs").at("operations").get_array();
    const auto& observations = scenario->at("expected").at("observations").get_array();
    REQUIRE(operations.size() == observations.size());
    std::vector<std::string> callback_events;
    cch::tui::SelectList list(items,
            cch::tui::SelectListOptions{
                    .on_select = [&callback_events](const auto& item) -> cch::support::ExpectedVoid {
                        callback_events.push_back("select:" + item.value);
                        return {};
                    },
                    .on_selection_change = [&callback_events](const auto& item) -> cch::support::ExpectedVoid {
                        callback_events.push_back("change:" + item.value);
                        return {};
                    },
            });
    for (std::size_t index = 0; index < operations.size(); ++index) {
        const auto& operation = operations[index];
        const auto& recorded = observations[index];
        const auto kind = operation.at("kind").get_string();
        if (kind == "filter") {
            list.set_filter(operation.at("value").get_string());
        } else {
            list.set_selected_index(static_cast<std::size_t>(operation.at("index").get_number()));
        }
        const auto rendered = list.render(60);
        REQUIRE(rendered);
        const auto selected = list.selected_item();
        if (recorded.at("selected").holds<std::nullptr_t>()) {
            CHECK_FALSE(selected.has_value());
        } else {
            REQUIRE(selected);
            CHECK(selected->value == recorded.at("selected").get_string());
        }
        CHECK(rendered->lines == string_array(recorded.at("rows")));
        const auto filtered = list.filtered_items();
        std::vector<std::string> filtered_values;
        for (const auto& item : filtered)
            filtered_values.push_back(item.value);
        CHECK(filtered_values == string_array(recorded.at("filteredValues")));
    }
    CHECK(callback_events == string_array(scenario->at("expected").at("callbackEvents")));
}

TEST_CASE("pi-v1.3.0 SelectList Unicode description normalization matches frozen observation",
        "[tui][select-list][issue1219]") {
    const auto scenario = select_list_v130_scenario("select-list-unicode-description-normalization");
    REQUIRE(scenario);
    const auto items = select_items(scenario->at("inputs").at("items"));
    cch::tui::SelectList list(items);
    const auto& observations = scenario->at("expected").at("observations").get_array();
    for (std::size_t index = 0; index < observations.size(); ++index) {
        list.set_selected_index(index);
        const auto rendered = list.render(80);
        REQUIRE(rendered);
        REQUIRE(list.selected_item());
        const auto& recorded = observations[index];
        CHECK(list.selected_item()->value == recorded.at("selected").get_string());
        CHECK(rendered->lines == string_array(recorded.at("rows")));
        std::vector<std::size_t> widths;
        for (const auto& line : rendered->lines)
            widths.push_back(cch::tui::visible_width(line));
        std::vector<std::size_t> expected_widths;
        for (const auto& expected_width : recorded.at("visibleWidths").get_array())
            expected_widths.push_back(static_cast<std::size_t>(expected_width.get_number()));
        CHECK(widths == expected_widths);
    }
}

TEST_CASE("pi-v1.3.0 SelectList unbound key non-mutation matches frozen observation", "[tui][select-list][issue1219]") {
    const auto scenario = select_list_v130_scenario("select-list-unbound-key-forwarding");
    REQUIRE(scenario);
    cch::tui::SelectList list({{.value = "first", .label = "First"}, {.value = "second", .label = "Second"}});
    for (const auto& observation : scenario->at("expected").at("observations").get_array()) {
        REQUIRE(list.selected_item());
        CHECK(list.selected_item()->value == observation.at("before").get_string());
        const auto& input = observation.at("input").get_string();
        const auto key = input == "\x1b[6~" ? "pageDown" : input.c_str();
        CHECK(list.handle_input(cch::tui::KeyEvent{.key = key}) == cch::tui::InputAdmissionOutcome::Unhandled);
        CHECK(list.selected_item()->value == observation.at("after").get_string());
    }
    REQUIRE(list.selected_item());
    CHECK(list.selected_item()->value ==
            scenario->at("expected").at("observations").get_array().back().at("after").get_string());
}

TEST_CASE("pi-v1.3.0 SelectList primary column layout and custom truncation match frozen observation",
        "[tui][select-list][issue1219]") {
    const auto scenario = select_list_v130_scenario("select-list-primary-column-layout-and-custom-truncation");
    REQUIRE(scenario);
    struct TruncateRecord {
        std::string text;
        std::size_t max_width{0};
        std::size_t column_width{0};
        std::string item_value;
        bool is_selected{false};
    };
    std::vector<TruncateRecord> truncate_calls;
    cch::tui::SelectListLayoutOptions layout{
            .min_primary_column_width = 15,
            .max_primary_column_width = 25,
            .truncate_primary = [&truncate_calls](std::string text,
                                        std::size_t max_width,
                                        std::size_t column_width,
                                        const cch::tui::SelectItem& item,
                                        bool is_selected) -> std::string {
                truncate_calls.push_back({
                        .text = text,
                        .max_width = max_width,
                        .column_width = column_width,
                        .item_value = item.value,
                        .is_selected = is_selected,
                });
                return "[" + text + "]";
            },
    };
    std::vector<cch::tui::SelectItem> items = {
            {.value = "short", .label = "Short", .description = "Short description"},
            {.value = "medium-length", .label = "Medium Length", .description = "Medium description"},
            {
                    .value = "long-item-exceeding-columns",
                    .label = "Very Long Item Label Exceeding Maximum Primary Column Width",
                    .description = "Long description",
            },
    };
    cch::tui::SelectList list(std::move(items), cch::tui::SelectListOptions{.layout = std::move(layout)});
    const auto& observations = scenario->at("expected").at("observations").get_array();
    for (const auto& obs : observations) {
        const auto width = static_cast<std::size_t>(obs.at("width").get_number());
        const auto rendered = list.render(width);
        REQUIRE(rendered);
        CHECK(rendered->lines == string_array(obs.at("rows")));
        std::vector<std::size_t> widths;
        for (const auto& line : rendered->lines)
            widths.push_back(cch::tui::visible_width(line));
        std::vector<std::size_t> expected_widths;
        for (const auto& expected_width : obs.at("visibleWidths").get_array())
            expected_widths.push_back(static_cast<std::size_t>(expected_width.get_number()));
        CHECK(widths == expected_widths);
    }

    const auto& expected_calls = scenario->at("expected").at("truncateCalls").get_array();
    REQUIRE(truncate_calls.size() == expected_calls.size());
    for (std::size_t i = 0; i < truncate_calls.size(); ++i) {
        CHECK(truncate_calls[i].text == expected_calls[i].at("text").get_string());
        CHECK(truncate_calls[i].max_width == static_cast<std::size_t>(expected_calls[i].at("maxWidth").get_number()));
        CHECK(truncate_calls[i].column_width ==
                static_cast<std::size_t>(expected_calls[i].at("columnWidth").get_number()));
        CHECK(truncate_calls[i].item_value == expected_calls[i].at("itemValue").get_string());
        CHECK(truncate_calls[i].is_selected == expected_calls[i].at("isSelected").get_boolean());
    }
}

TEST_CASE("SelectList window centering rejects off-by-one indices and clamps bounds",
        "[tui][select-list][boundary][issue1219]") {
    std::vector<cch::tui::SelectItem> items;
    for (std::size_t i = 0; i < 9; ++i) {
        items.push_back({.value = "val-" + std::to_string(i), .label = "Label " + std::to_string(i)});
    }
    cch::tui::SelectList list(std::move(items), cch::tui::SelectListOptions{.max_visible = 5});
    // At index 0, window is 0..5
    auto r0 = list.render(40);
    REQUIRE(r0);
    CHECK(r0->lines.size() == 6); // 5 items + scroll indicator (1/9)
    CHECK(r0->lines[0].starts_with("→ Label 0"));
    CHECK(r0->lines[4].starts_with("  Label 4"));
    CHECK(r0->lines[5].find("(1/9)") != std::string::npos);

    // Out-of-bounds index clamps to count-1
    list.set_selected_index(999);
    REQUIRE(list.selected_item());
    CHECK(list.selected_item()->value == "val-8");
    auto r_end = list.render(40);
    REQUIRE(r_end);
    CHECK(r_end->lines.size() == 6);
    CHECK(r_end->lines[0].starts_with("  Label 4"));
    CHECK(r_end->lines[4].starts_with("→ Label 8"));
    CHECK(r_end->lines[5].find("(9/9)") != std::string::npos);

    // List smaller than max_visible renders without scroll indicator
    cch::tui::SelectList small_list({{.value = "a", .label = "A"}, {.value = "b", .label = "B"}},
            cch::tui::SelectListOptions{.max_visible = 5});
    auto r_small = small_list.render(40);
    REQUIRE(r_small);
    CHECK(r_small->lines.size() == 2);
}

TEST_CASE("SelectList layout bounds clamp inverted minimum and maximum options",
        "[tui][select-list][boundary][issue1219]") {
    cch::tui::SelectListLayoutOptions inverted_layout{
            .min_primary_column_width = 30,
            .max_primary_column_width = 10,
    };
    cch::tui::SelectList list(
            {{.value = "a", .label = "Short"}}, cch::tui::SelectListOptions{.layout = std::move(inverted_layout)});
    auto rendered = list.render(60);
    REQUIRE(rendered);
    REQUIRE_FALSE(rendered->lines.empty());
    CHECK(cch::tui::visible_width(rendered->lines[0]) <= 60);

    // Width 0 fails validation closed
    auto invalid_render = list.render(0);
    CHECK_FALSE(invalid_render.has_value());
}

TEST_CASE(
        "SelectList description truncation never cuts multi-byte graphemes and omits descriptions under minimum width",
        "[tui][select-list][boundary][issue1219]") {
    std::vector<cch::tui::SelectItem> items = {
            {.value = "cjk", .label = "CJK", .description = "日本語の長い説明文テキスト"},
    };
    cch::tui::SelectList list(std::move(items));
    // At width 40, description is omitted because width <= 40
    auto r40 = list.render(40);
    REQUIRE(r40);
    CHECK(r40->lines[0] == "→ CJK");

    // At width 45, space is narrow and description is truncated cleanly
    auto r45 = list.render(45);
    REQUIRE(r45);
    CHECK(r45->lines[0].starts_with("→ CJK"));
    CHECK(cch::tui::visible_width(r45->lines[0]) <= 45);
}

TEST_CASE("SelectList filtering resets selection to zero and handles empty item collections",
        "[tui][select-list][boundary][issue1219]") {
    std::vector<cch::tui::SelectItem> items = {
            {.value = "apple", .label = "Apple"},
            {.value = "apricot", .label = "Apricot"},
            {.value = "banana", .label = "Banana"},
    };
    cch::tui::SelectList list(std::move(items));
    list.set_selected_index(2);
    REQUIRE(list.selected_item());
    CHECK(list.selected_item()->value == "banana");

    // Setting filter resets selection to 0
    list.set_filter("ap");
    REQUIRE(list.selected_item());
    CHECK(list.selected_item()->value == "apple");

    // Non-matching filter empties items and renders no-match
    list.set_filter("xyz");
    CHECK_FALSE(list.selected_item().has_value());
    auto rendered = list.render(40);
    REQUIRE(rendered);
    CHECK(rendered->lines.size() == 1);
    CHECK(rendered->lines[0].find("No matching commands") != std::string::npos);

    // Empty initial list
    cch::tui::SelectList empty_list({});
    auto empty_render = empty_list.render(40);
    REQUIRE(empty_render);
    CHECK(empty_render->lines.size() == 1);
    CHECK(empty_render->lines[0].find("No matching commands") != std::string::npos);
}

TEST_CASE("SelectList unbound keys return unhandled and do not fire selection callbacks",
        "[tui][select-list][negative][issue1219]") {
    std::vector<std::string> selects;
    std::vector<std::string> changes;
    cch::tui::SelectList list({{.value = "one", .label = "One"}, {.value = "two", .label = "Two"}},
            cch::tui::SelectListOptions{
                    .on_select = [&selects](const auto& item) -> cch::support::ExpectedVoid {
                        selects.push_back(item.value);
                        return {};
                    },
                    .on_selection_change = [&changes](const auto& item) -> cch::support::ExpectedVoid {
                        changes.push_back(item.value);
                        return {};
                    },
            });
    CHECK(list.handle_input(cch::tui::KeyEvent{.key = "x"}) == cch::tui::InputAdmissionOutcome::Unhandled);
    CHECK(list.handle_input(cch::tui::KeyEvent{.key = "f12"}) == cch::tui::InputAdmissionOutcome::Unhandled);
    CHECK(list.handle_input(cch::tui::KeyEvent{.key = "pageDown"}) == cch::tui::InputAdmissionOutcome::Unhandled);
    CHECK(selects.empty());
    CHECK(changes.empty());
    REQUIRE(list.selected_item());
    CHECK(list.selected_item()->value == "one");
}

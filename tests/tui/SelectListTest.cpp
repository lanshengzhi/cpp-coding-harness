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
    overlay_options.size_constraints.max_height = 3;
    auto overlay = std::make_unique<cch::tui::Overlay>(std::move(overlay_options));
    auto list = std::make_unique<cch::tui::SelectList>(make_items(8), cch::tui::SelectListOptions{.max_visible = 2});
    REQUIRE(overlay->add_child(std::move(list)));
    REQUIRE(overlay->focus_first());
    auto* overlay_ptr = overlay.get();
    REQUIRE(tui.add_overlay(std::move(overlay)));
    REQUIRE(tui.start());
    REQUIRE(tui.set_focus(overlay_ptr));
    REQUIRE(tui.render());

    CHECK(terminal.screen()[0].find("Item 0") != std::string::npos);
    CHECK(terminal.screen()[0].find("description 0") != std::string::npos);
    CHECK(terminal.screen()[2].find("(1/8)") != std::string::npos);
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

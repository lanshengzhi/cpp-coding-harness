#include <cch/tui/Overlay.hpp>
#include <cch/tui/SelectList.hpp>
#include <cch/tui/Tui.hpp>
#include <cch/tui/VirtualTerminal.hpp>
#include <cch/tui/Utils.hpp>

#include <catch2/catch_test_macros.hpp>

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

TEST_CASE("SelectList renders empty and unmatched filters through VirtualTerminal", "[tui][select-list][issue52]") {
    cch::tui::VirtualTerminal terminal({.columns = 30, .rows = 3});
    cch::tui::Tui tui(terminal);
    auto list = std::make_unique<cch::tui::SelectList>(std::vector<cch::tui::SelectItem>{});
    auto* list_ptr = list.get();
    REQUIRE(tui.add_child(std::move(list)));
    REQUIRE(tui.start());
    REQUIRE(tui.set_focus(list_ptr));
    REQUIRE(tui.render());
    CHECK(terminal.screen()[0].find("No matching commands") != std::string::npos);

    list_ptr->set_filter("missing");
    REQUIRE(tui.render());
    CHECK(terminal.screen()[0].find("No matching commands") != std::string::npos);
}

TEST_CASE("SelectList pages large bounded lists and adapts descriptions after resize", "[tui][select-list][issue52]") {
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

    REQUIRE(terminal.inject_input("\x1b[6~"));
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

TEST_CASE("SelectList filters navigates wraps selects and cancels with semantic keys", "[tui][select-list][issue52]") {
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
            .on_select = [&selected](const cch::tui::SelectItem& item) { selected = item.value; },
            .on_cancel = [&cancellations]() { ++cancellations; },
            .on_selection_change = [&changed](const cch::tui::SelectItem& item) { changed = item.value; },
        });

    list.set_filter("al");
    list.handle_input(cch::tui::KeyEvent{.key = "up"});
    REQUIRE(list.selected_item());
    CHECK(list.selected_item()->value == "alpine");
    CHECK(changed == "alpine");
    list.handle_input(cch::tui::KeyEvent{.key = "enter"});
    CHECK(selected == "alpine");
    list.handle_input(cch::tui::KeyEvent{.key = "escape"});
    CHECK(cancellations == 1);
}

TEST_CASE("SelectList normalizes and aligns descriptions within configured columns", "[tui][select-list][issue52]") {
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

TEST_CASE("SelectList dispatches configured keys from its effective registry", "[tui][select-list][issue57]") {
    cch::tui::KeybindingResolutionRequest request;
    request.definitions = cch::tui::builtin_tui_keybinding_definitions();
    request.overrides = {{.id = "tui.select.confirm", .keys = {"f2"}}};
    const auto keybindings = cch::tui::resolve_keybindings(std::move(request));
    REQUIRE(keybindings);

    std::size_t selections = 0;
    cch::tui::SelectList list(
        make_items(1),
        cch::tui::SelectListOptions{
            .on_select = [&selections](const cch::tui::SelectItem&) { ++selections; },
            .keybindings = keybindings->registry,
        });
    list.handle_input(cch::tui::KeyEvent{.key = "enter"});
    CHECK(selections == 0);
    list.handle_input(cch::tui::KeyEvent{.key = "f2"});
    CHECK(selections == 1);
}

TEST_CASE("SelectList renders descriptions in an overlay with bounded rows", "[tui][select-list][overlay][issue52]") {
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

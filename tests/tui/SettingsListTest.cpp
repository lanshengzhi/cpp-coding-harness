#include <cch/tui/SelectList.hpp>
#include <cch/tui/SettingsList.hpp>
#include <cch/tui/Tui.hpp>
#include <cch/tui/VirtualTerminal.hpp>

#include "../../third_party/catch2/catch_test_macros.hpp"

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

TEST_CASE("SettingsList cycles values deterministically and reports updates", "[tui][settings-list][issue52]") {
    std::vector<std::string> updates;
    cch::tui::SettingsList list(
        {
            {
                .id = "theme",
                .label = "Theme",
                .description = "Color theme",
                .current_value = "unknown",
                .control = cch::tui::SettingValues{{"dark", "light"}},
            },
        },
        cch::tui::SettingsListOptions{
            .on_change = [&updates](std::string id, std::string value) {
                updates.push_back(std::move(id) + "=" + std::move(value));
            },
        });

    list.handle_input(cch::tui::KeyEvent{.key = "enter"});
    REQUIRE(list.selected_item());
    CHECK(list.selected_item()->current_value == "dark");
    list.handle_input(cch::tui::KeyEvent{.key = "space"});
    REQUIRE(list.selected_item());
    CHECK(list.selected_item()->current_value == "light");
    REQUIRE(updates.size() == 2);
    CHECK(updates[0] == "theme=dark");
    CHECK(updates[1] == "theme=light");
}

TEST_CASE("SettingsList search and no matches render through VirtualTerminal", "[tui][settings-list][issue52]") {
    cch::tui::VirtualTerminal terminal({.columns = 50, .rows = 8});
    cch::tui::Tui tui(terminal);
    auto list = std::make_unique<cch::tui::SettingsList>(
        std::vector<cch::tui::SettingItem>{
            {.id = "theme", .label = "Theme", .current_value = "dark"},
            {.id = "tools", .label = "Active tools", .current_value = "read"},
            {.id = "thinking", .label = "Thinking level", .current_value = "low"},
        },
        cch::tui::SettingsListOptions{.max_visible = 2, .enable_search = true});
    auto* list_ptr = list.get();
    REQUIRE(tui.add_child(std::move(list)));
    REQUIRE(tui.start());
    REQUIRE(tui.set_focus(list_ptr));
    REQUIRE(terminal.inject_input("tool"));
    REQUIRE(tui.render());
    CHECK(list_ptr->search_query() == "tool");
    REQUIRE(list_ptr->selected_item());
    CHECK(list_ptr->selected_item()->id == "tools");

    REQUIRE(terminal.inject_input("z"));
    REQUIRE(tui.render());
    CHECK(terminal.screen()[2].find("No matching settings") != std::string::npos);
    CHECK(list_ptr->focused());
}

TEST_CASE("SettingsList search preserves baseline token and alphanumeric matching", "[tui][settings-list][issue52]") {
    cch::tui::SettingsList token_search(
        {
            {.id = "theme", .label = "Theme", .current_value = "dark"},
            {.id = "tools", .label = "Active tools", .current_value = "read"},
        },
        cch::tui::SettingsListOptions{.enable_search = true});
    for (const auto& key : std::string("active/tool")) {
        token_search.handle_input(cch::tui::KeyEvent{.key = std::string(1, key)});
    }
    REQUIRE(token_search.selected_item());
    CHECK(token_search.selected_item()->id == "tools");

    cch::tui::SettingsList alphanumeric_search(
        {
            {.id = "other", .label = "Other", .current_value = "off"},
            {.id = "model", .label = "4 GPT", .current_value = "on"},
        },
        cch::tui::SettingsListOptions{.enable_search = true});
    for (const auto& key : std::string("gpt4")) {
        alphanumeric_search.handle_input(cch::tui::KeyEvent{.key = std::string(1, key)});
    }
    REQUIRE(alphanumeric_search.selected_item());
    CHECK(alphanumeric_search.selected_item()->id == "model");

    cch::tui::SettingsList unicode_search(
        {
            {.id = "plain", .label = "Plain", .current_value = "off"},
            {.id = "accented", .label = "Éclair", .current_value = "on"},
        },
        cch::tui::SettingsListOptions{.enable_search = true});
    unicode_search.handle_input(cch::tui::KeyEvent{.key = "é"});
    REQUIRE(unicode_search.selected_item());
    CHECK(unicode_search.selected_item()->id == "accented");
}

TEST_CASE(
    "SettingsList delegates nested selection and restores its parent selection",
    "[tui][settings-list][issue52]") {
    std::vector<std::string> updates;
    cch::tui::SettingsList list(
        {
            {
                .id = "plain",
                .label = "Plain",
                .current_value = "off",
                .control = cch::tui::SettingValues{{"off", "on"}},
            },
            {.id = "theme", .label = "Theme", .current_value = "dark", .control = cch::tui::SettingSubmenu{}},
        },
        cch::tui::SettingsListOptions{
            .on_change = [&updates](std::string id, std::string value) {
                updates.push_back(std::move(id) + "=" + std::move(value));
            },
            .submenu_factory = [](const cch::tui::SettingItem&, cch::tui::SettingsSubmenuDoneSink done) {
                auto shared_done = std::make_shared<cch::tui::SettingsSubmenuDoneSink>(std::move(done));
                return std::make_unique<cch::tui::SelectList>(
                    std::vector<cch::tui::SelectItem>{
                        {.value = "dark", .label = "Dark"},
                        {.value = "light", .label = "Light"},
                    },
                    cch::tui::SelectListOptions{
                        .on_select = [shared_done](const cch::tui::SelectItem& item) {
                            (*shared_done)(item.value);
                        },
                        .on_cancel = [shared_done]() { (*shared_done)(std::nullopt); },
                    });
            },
        });

    list.handle_input(cch::tui::KeyEvent{.key = "down"});
    list.handle_input(cch::tui::KeyEvent{.key = "enter"});
    REQUIRE(list.submenu_open());
    list.handle_input(cch::tui::KeyEvent{.key = "down"});
    list.handle_input(cch::tui::KeyEvent{.key = "enter"});

    CHECK_FALSE(list.submenu_open());
    REQUIRE(list.selected_item());
    CHECK(list.selected_item()->id == "theme");
    CHECK(list.selected_item()->current_value == "light");
    REQUIRE(updates.size() == 1);
    CHECK(updates[0] == "theme=light");
}

TEST_CASE("SettingsList cancellation invokes the callback once per semantic key", "[tui][settings-list][issue52]") {
    std::size_t cancellations = 0;
    cch::tui::SettingsList list({}, cch::tui::SettingsListOptions{
        .on_cancel = [&cancellations]() { ++cancellations; },
    });
    list.handle_input(cch::tui::KeyEvent{.key = "escape"});
    CHECK(cancellations == 1);
}

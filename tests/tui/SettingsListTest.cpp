#include <cch/tui/SelectList.hpp>
#include <cch/tui/SettingsList.hpp>
#include <cch/tui/Tui.hpp>
#include <cch/tui/Utils.hpp>
#include <cch/tui/VirtualTerminal.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

TEST_CASE(
    "SettingsList cycles values deterministically and reports updates",
    "[tui][settings-list][issue52][issue57]") {
    cch::tui::KeybindingResolutionRequest request;
    request.definitions = cch::tui::builtin_tui_keybinding_definitions();
    request.overrides = {{.id = "tui.select.confirm", .keys = {"enter", "space"}}};
    const auto keybindings = cch::tui::resolve_keybindings(std::move(request));
    REQUIRE(keybindings);

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
            .on_change = [&updates](std::string id, std::string value) -> cch::support::ExpectedVoid {
                updates.push_back(std::move(id) + "=" + std::move(value));
                return {};
            },
            .keybindings = keybindings->registry,
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
            .on_change = [&updates](std::string id, std::string value) -> cch::support::ExpectedVoid {
                updates.push_back(std::move(id) + "=" + std::move(value));
                return {};
            },
            .submenu_factory = [](const cch::tui::SettingItem&, cch::tui::SettingsSubmenuDoneSink done) {
                auto shared_done = std::make_shared<cch::tui::SettingsSubmenuDoneSink>(std::move(done));
                return std::make_unique<cch::tui::SelectList>(
                    std::vector<cch::tui::SelectItem>{
                        {.value = "dark", .label = "Dark"},
                        {.value = "light", .label = "Light"},
                    },
                    cch::tui::SelectListOptions{
                        .on_select = [shared_done](const cch::tui::SelectItem& item) -> cch::support::ExpectedVoid {
                            (void)(*shared_done)(item.value);
                            return {};
                        },
                        .on_cancel = [shared_done]() -> cch::support::ExpectedVoid {
                            (void)(*shared_done)(std::nullopt);
                            return {};
                        },
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

TEST_CASE("SettingsList renders and dispatches hints from one effective registry", "[tui][settings-list][issue57]") {
    cch::tui::KeybindingResolutionRequest request;
    request.definitions = cch::tui::builtin_tui_keybinding_definitions();
    request.overrides = {
        {.id = "tui.select.confirm", .keys = {"f2"}},
        {.id = "tui.select.cancel", .keys = {"f3"}},
    };
    const auto keybindings = cch::tui::resolve_keybindings(std::move(request));
    REQUIRE(keybindings);

    std::size_t changes = 0;
    std::size_t cancellations = 0;
    cch::tui::SettingsList list(
        {{
            .id = "theme",
            .label = "Theme",
            .current_value = "dark",
            .control = cch::tui::SettingValues{{"dark", "light"}},
        }},
        cch::tui::SettingsListOptions{
            .on_change = [&changes](std::string, std::string) -> cch::support::ExpectedVoid { ++changes; return {}; },
            .on_cancel = [&cancellations]() -> cch::support::ExpectedVoid { ++cancellations; return {}; },
            .keybindings = keybindings->registry,
        });
    const auto rendered = list.render(60);
    REQUIRE(rendered);
    CHECK(std::any_of(rendered->lines.begin(), rendered->lines.end(), [](const auto& line) {
        return line.find("f2/space to change · f3 to cancel") != std::string::npos;
    }));
    list.handle_input(cch::tui::KeyEvent{.key = "enter"});
    list.handle_input(cch::tui::KeyEvent{.key = "escape"});
    CHECK(changes == 0);
    CHECK(cancellations == 0);
    list.handle_input(cch::tui::KeyEvent{.key = "f2"});
    list.handle_input(cch::tui::KeyEvent{.key = "f3"});
    CHECK(changes == 1);
    CHECK(cancellations == 1);
}

TEST_CASE("SettingsList space confirms only while the search is empty", "[tui][settings-list][issue384]") {
    std::vector<std::string> updates;
    cch::tui::SettingsList list(
        {
            {
                .id = "theme",
                .label = "Theme",
                .current_value = "off",
                .control = cch::tui::SettingValues{{"off", "on"}},
            },
        },
        cch::tui::SettingsListOptions{
            .enable_search = true,
            .on_change = [&updates](std::string id, std::string value) -> cch::support::ExpectedVoid {
                updates.push_back(std::move(id) + "=" + std::move(value));
                return {};
            },
        });

    // Space with an empty search activates the item (pi settings-list.ts).
    list.handle_input(cch::tui::KeyEvent{.key = "space"});
    REQUIRE(list.selected_item());
    CHECK(list.selected_item()->current_value == "on");
    CHECK(updates.size() == 1);

    // Space with a non-empty search inserts a space into the query instead.
    list.handle_input(cch::tui::KeyEvent{.key = "t"});
    list.handle_input(cch::tui::KeyEvent{.key = "space"});
    CHECK(list.search_query() == "t ");
    CHECK(updates.size() == 1);
}

TEST_CASE("SettingsList search editing flows through the Input component", "[tui][settings-list][issue384]") {
    cch::tui::SettingsList list(
        {
            {.id = "alpha", .label = "Alpha", .current_value = "off"},
            {.id = "beta", .label = "Beta", .current_value = "off"},
        },
        cch::tui::SettingsListOptions{.enable_search = true});
    const auto key = [&list](std::string identifier, bool ctrl = false) {
        list.handle_input(cch::tui::KeyEvent{.key = std::move(identifier), .ctrl = ctrl});
    };
    const auto type = [&key](std::string_view text) {
        for (const auto& character : text) key(std::string(1, character));
    };

    type("al");
    REQUIRE(list.selected_item());
    CHECK(list.selected_item()->id == "alpha");

    // Cursor movement and mid-query insertion are the Input component's
    // editing behaviors; the filter re-runs on the component's full value.
    key("left");
    key("p");
    CHECK(list.search_query() == "apl");
    CHECK_FALSE(list.selected_item());

    // deleteCharBackward and undo resolve in the same effective registry.
    key("backspace");
    CHECK(list.search_query() == "al");
    key("-", true);
    CHECK(list.search_query() == "apl");
    CHECK_FALSE(list.selected_item());
}

TEST_CASE("SettingsList paste flows through the Input component's cleaning", "[tui][settings-list][issue384]") {
    cch::tui::SettingsList list(
        {
            {.id = "alpha", .label = "Alpha beta", .current_value = "off"},
            {.id = "beta", .label = "Beta", .current_value = "off"},
        },
        cch::tui::SettingsListOptions{.enable_search = true});

    // CRLF is dropped and spaces are preserved by the Input component's paste
    // cleaning; the query updates and the filter re-runs on it.
    list.handle_input(cch::tui::PasteEvent{.text = "al\r\nph"});
    CHECK(list.search_query() == "alph");
    REQUIRE(list.selected_item());
    CHECK(list.selected_item()->id == "alpha");

    list.handle_input(cch::tui::PasteEvent{.text = "a b"});
    CHECK(list.search_query() == "alpha b");
    REQUIRE(list.selected_item());
    CHECK(list.selected_item()->id == "alpha");
}

TEST_CASE("SettingsList search line renders and locates the cursor through Input", "[tui][settings-list][issue384]") {
    cch::tui::SettingsList list(
        {
            {.id = "alpha", .label = "Alpha", .current_value = "off"},
        },
        cch::tui::SettingsListOptions{.enable_search = true});
    for (const auto& character : std::string("tool")) {
        list.handle_input(cch::tui::KeyEvent{.key = std::string(1, character)});
    }
    list.set_focused(true);
    const auto rendered = list.render(50);
    REQUIRE(rendered);
    REQUIRE(rendered->lines.size() >= 2);
    CHECK(rendered->lines[0].starts_with("> tool"));
    CHECK(cch::tui::visible_width(rendered->lines[0]) == 50);
    CHECK(rendered->lines[1].empty());

    const auto cursor = list.cursor_location();
    REQUIRE(cursor);
    CHECK(cursor->row == 0);
    CHECK(cursor->column == 6);
}

TEST_CASE("SettingsList cancellation invokes the callback once per semantic key", "[tui][settings-list][issue52]") {
    std::size_t cancellations = 0;
    cch::tui::SettingsList list({}, cch::tui::SettingsListOptions{
        .on_cancel = [&cancellations]() -> cch::support::ExpectedVoid { ++cancellations; return {}; },
    });
    list.handle_input(cch::tui::KeyEvent{.key = "escape"});
    CHECK(cancellations == 1);
}

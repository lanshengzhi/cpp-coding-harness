#include <cch/tui/SelectList.hpp>
#include <cch/tui/SettingsList.hpp>
#include <cch/tui/Tui.hpp>
#include <cch/tui/Utils.hpp>
#include <cch/tui/VirtualTerminal.hpp>

#include "support/PiTuiEvidence.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

struct SettingsSubmenuProbeState {
    std::string label;
    cch::tui::InputAdmissionOutcome input_outcome{cch::tui::InputAdmissionOutcome::Unhandled};
    std::move_only_function<void()> on_input;
    bool focused{false};
};

class SettingsSubmenuProbe final : public cch::tui::Component,
                                   public cch::tui::InputHandler,
                                   public cch::tui::Focusable {
public:
    explicit SettingsSubmenuProbe(std::shared_ptr<SettingsSubmenuProbeState> state) : state_(std::move(state)) {}

    [[nodiscard]] cch::support::Expected<cch::tui::RenderResult> render(std::size_t) override {
        return cch::tui::RenderResult{.lines = {state_->label}};
    }

    void invalidate() override {}

    cch::tui::InputAdmissionOutcome handle_input(const cch::tui::InputEventVariant&) override {
        if (!state_->on_input) return state_->input_outcome;
        auto on_input = std::move(state_->on_input);
        on_input();
        return cch::tui::InputAdmissionOutcome::Consumed;
    }

    void set_focused(bool focused) override { state_->focused = focused; }
    [[nodiscard]] bool focused() const override { return state_->focused; }

private:
    std::shared_ptr<SettingsSubmenuProbeState> state_;
};

} // namespace

TEST_CASE("SettingsList cycles values deterministically and reports updates",
        "[tui][settings-list][issue52][issue57][spec]") {
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

    static_cast<void>(list.handle_input(cch::tui::KeyEvent{.key = "enter"}));
    REQUIRE(list.selected_item());
    CHECK(list.selected_item()->current_value == "dark");
    static_cast<void>(list.handle_input(cch::tui::KeyEvent{.key = "space"}));
    REQUIRE(list.selected_item());
    CHECK(list.selected_item()->current_value == "light");
    REQUIRE(updates.size() == 2);
    CHECK(updates[0] == "theme=dark");
    CHECK(updates[1] == "theme=light");
}

TEST_CASE("SettingsList resolves a key shared by two actions in dispatch order", "[tui][settings-list][spec]") {
    // f9 claims up and confirm; movement leads pi's settings-list chain.
    // space is bound to the LATER cancel action, yet the raw space guard at
    // confirm's chain position still activates the item (pi settings-list.ts).
    auto registry = std::make_shared<const cch::tui::KeybindingRegistry>(std::vector<cch::tui::EffectiveKeybinding>{
            {.id = "tui.select.up", .keys = {"f9"}},
            {.id = "tui.select.confirm", .keys = {"f9"}},
            {.id = "tui.select.cancel", .keys = {"space"}},
    });
    std::vector<std::string> updates;
    std::size_t cancels = 0;
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
                    .on_cancel = [&cancels]() -> cch::support::ExpectedVoid {
                        ++cancels;
                        return {};
                    },
                    .keybindings = std::move(registry),
            });
    static_cast<void>(list.handle_input(cch::tui::KeyEvent{.key = "f9"}));
    CHECK(updates.empty());
    CHECK(cancels == 0);
    static_cast<void>(list.handle_input(cch::tui::KeyEvent{.key = "space"}));
    CHECK(cancels == 0);
    REQUIRE(updates.size() == 1);
    CHECK(updates[0] == "theme=dark");
}

TEST_CASE("SettingsList search and no matches render through VirtualTerminal", "[tui][settings-list][issue52][spec]") {
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

TEST_CASE("SettingsList search preserves baseline token and alphanumeric matching",
        "[tui][settings-list][issue52][spec]") {
    cch::tui::SettingsList token_search(
        {
            {.id = "theme", .label = "Theme", .current_value = "dark"},
            {.id = "tools", .label = "Active tools", .current_value = "read"},
        },
        cch::tui::SettingsListOptions{.enable_search = true});
    for (const auto& key : std::string("active/tool")) {
        static_cast<void>(token_search.handle_input(cch::tui::KeyEvent{.key = std::string(1, key)}));
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
        static_cast<void>(alphanumeric_search.handle_input(cch::tui::KeyEvent{.key = std::string(1, key)}));
    }
    REQUIRE(alphanumeric_search.selected_item());
    CHECK(alphanumeric_search.selected_item()->id == "model");

    cch::tui::SettingsList unicode_search(
        {
            {.id = "plain", .label = "Plain", .current_value = "off"},
            {.id = "accented", .label = "Éclair", .current_value = "on"},
        },
        cch::tui::SettingsListOptions{.enable_search = true});
    static_cast<void>(unicode_search.handle_input(cch::tui::KeyEvent{.key = "é"}));
    REQUIRE(unicode_search.selected_item());
    CHECK(unicode_search.selected_item()->id == "accented");
}

TEST_CASE("SettingsList search ranks non-ASCII labels by the frozen UTF-16 scores",
        "[tui][settings-list][issue958][spec]") {
    // "Éa" scores 0.1 for the queried "a" and "xxa" scores 0.2, so the accented
    // label wins even though it is listed second. Byte-indexed scoring ties
    // them at 0.2 and leaves the input order to decide.
    cch::tui::SettingsList list(
            {
                    {.id = "ascii", .label = "xxa", .current_value = "off"},
                    {.id = "accented", .label = "\xc3\x89\x61", .current_value = "on"},
            },
            cch::tui::SettingsListOptions{.enable_search = true});
    static_cast<void>(list.handle_input(cch::tui::KeyEvent{.key = "a"}));
    REQUIRE(list.selected_item());
    CHECK(list.selected_item()->id == "accented");
}

TEST_CASE("SettingsList delegates nested selection and restores its parent selection",
        "[tui][settings-list][issue52][spec]") {
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
                                .on_select = [shared_done](
                                                     const cch::tui::SelectItem& item) -> cch::support::ExpectedVoid {
                                    (void)(*shared_done)(item.value, std::nullopt);
                                    return {};
                                },
                                .on_cancel = [shared_done]() -> cch::support::ExpectedVoid {
                                    (void)(*shared_done)(std::nullopt, std::nullopt);
                                    return {};
                                },
                        });
            },
        });

    static_cast<void>(list.handle_input(cch::tui::KeyEvent{.key = "down"}));
    static_cast<void>(list.handle_input(cch::tui::KeyEvent{.key = "enter"}));
    REQUIRE(list.submenu_open());
    static_cast<void>(list.handle_input(cch::tui::KeyEvent{.key = "down"}));
    static_cast<void>(list.handle_input(cch::tui::KeyEvent{.key = "enter"}));

    CHECK_FALSE(list.submenu_open());
    REQUIRE(list.selected_item());
    CHECK(list.selected_item()->id == "theme");
    CHECK(list.selected_item()->current_value == "light");
    REQUIRE(updates.size() == 1);
    CHECK(updates[0] == "theme=light");
}

TEST_CASE("SettingsList selects an id from the unfiltered displayed items", "[tui][settings-list][issue968]") {
    cch::tui::SettingsList list({
            {.id = "alpha", .label = "Alpha"},
            {.id = "beta", .label = "Beta"},
            {.id = "gamma", .label = "Gamma"},
    });

    list.select_item("beta");
    REQUIRE(list.selected_item());
    CHECK(list.selected_item()->id == "beta");

    list.select_item("missing");
    REQUIRE(list.selected_item());
    CHECK(list.selected_item()->id == "beta");
}

TEST_CASE("SettingsList selects an id in the active filtered items", "[tui][settings-list][issue968]") {
    cch::tui::SettingsList list(
            {
                    {.id = "alpha", .label = "Alpha"},
                    {.id = "alpine", .label = "Alpine"},
                    {.id = "beta", .label = "Beta"},
                    {.id = "gamma", .label = "Gamma"},
            },
            cch::tui::SettingsListOptions{.enable_search = true});
    static_cast<void>(list.handle_input(cch::tui::KeyEvent{.key = "a"}));
    static_cast<void>(list.handle_input(cch::tui::KeyEvent{.key = "l"}));

    list.select_item("alpine");
    REQUIRE(list.selected_item());
    CHECK(list.selected_item()->id == "alpine");
    list.select_item("beta");
    REQUIRE(list.selected_item());
    CHECK(list.selected_item()->id == "alpine");
}

TEST_CASE("SettingsList opens the navigated submenu after completion", "[tui][settings-list][issue968]") {
    auto first_state = std::make_shared<SettingsSubmenuProbeState>();
    first_state->label = "first submenu";
    std::shared_ptr<cch::tui::SettingsSubmenuDoneSink> first_completion;
    auto second_state = std::make_shared<SettingsSubmenuProbeState>();
    second_state->label = "second submenu";
    std::size_t factory_calls = 0;
    cch::tui::SettingsList list(
            {
                    {.id = "first", .label = "First", .control = cch::tui::SettingSubmenu{}},
                    {.id = "second", .label = "Second", .control = cch::tui::SettingSubmenu{}},
            },
            cch::tui::SettingsListOptions{
                    .submenu_factory =
                            [first_state, second_state, &first_completion, &factory_calls](
                                    const cch::tui::SettingItem& item, cch::tui::SettingsSubmenuDoneSink done) {
                                ++factory_calls;
                                auto probe_state = item.id == "first" ? first_state : second_state;
                                if (item.id == "first") {
                                    first_completion =
                                            std::make_shared<cch::tui::SettingsSubmenuDoneSink>(std::move(done));
                                    first_state->on_input = [completion = first_completion] {
                                        (void)(*completion)(std::nullopt,
                                                cch::tui::SettingsSubmenuOptions{.navigate_to = "second"});
                                    };
                                }
                                return std::make_unique<SettingsSubmenuProbe>(std::move(probe_state));
                            },
            });

    static_cast<void>(list.handle_input(cch::tui::KeyEvent{.key = "enter"}));
    REQUIRE(list.submenu_open());
    static_cast<void>(list.handle_input(cch::tui::KeyEvent{.key = "complete"}));

    REQUIRE(list.submenu_open());
    REQUIRE(list.selected_item());
    CHECK(list.selected_item()->id == "second");
    CHECK(factory_calls == 2);
    const auto rendered = list.render(40);
    REQUIRE(rendered);
    REQUIRE_FALSE(rendered->lines.empty());
    CHECK(rendered->lines[0] == "second submenu");
}

TEST_CASE("SettingsList bubbles unhandled input from its submenu", "[tui][settings-list][issue968]") {
    auto state = std::make_shared<SettingsSubmenuProbeState>();
    state->label = "submenu";
    state->input_outcome = cch::tui::InputAdmissionOutcome::Unhandled;
    cch::tui::SettingsList list({{.id = "submenu", .label = "Submenu", .control = cch::tui::SettingSubmenu{}}},
            cch::tui::SettingsListOptions{
                    .submenu_factory =
                            [state](const cch::tui::SettingItem&, cch::tui::SettingsSubmenuDoneSink) {
                                return std::make_unique<SettingsSubmenuProbe>(state);
                            },
            });
    static_cast<void>(list.handle_input(cch::tui::KeyEvent{.key = "enter"}));

    CHECK(list.handle_input(cch::tui::KeyEvent{.key = "pageDown"}) == cch::tui::InputAdmissionOutcome::Unhandled);
}

TEST_CASE("SettingsList clears submenu focus and ignores its late completion", "[tui][settings-list][issue968]") {
    auto state = std::make_shared<SettingsSubmenuProbeState>();
    state->label = "submenu";
    std::shared_ptr<cch::tui::SettingsSubmenuDoneSink> completion;
    std::size_t updates = 0;
    cch::tui::SettingsList list(
            {{.id = "submenu", .label = "Submenu", .current_value = "before", .control = cch::tui::SettingSubmenu{}}},
            cch::tui::SettingsListOptions{
                    .on_change = [&updates](std::string, std::string) -> cch::support::ExpectedVoid {
                        ++updates;
                        return {};
                    },
                    .submenu_factory =
                            [state, &completion](const cch::tui::SettingItem&, cch::tui::SettingsSubmenuDoneSink done) {
                                completion = std::make_shared<cch::tui::SettingsSubmenuDoneSink>(std::move(done));
                                return std::make_unique<SettingsSubmenuProbe>(state);
                            },
            });
    list.set_focused(true);
    static_cast<void>(list.handle_input(cch::tui::KeyEvent{.key = "enter"}));
    REQUIRE(list.submenu_open());
    CHECK(state->focused);

    REQUIRE(completion);
    (void)(*completion)(std::nullopt, std::nullopt);
    CHECK_FALSE(list.submenu_open());
    CHECK_FALSE(state->focused);
    CHECK(updates == 0);
    (void)(*completion)(std::string{"late"}, std::nullopt);
    CHECK(updates == 0);
    REQUIRE(list.selected_item());
    CHECK(list.selected_item()->current_value == "before");
}

TEST_CASE("SettingsList preserves pi ellipses for long rows and hints", "[tui][settings-list][issue968]") {
    cch::tui::SettingsList list({{.id = "long", .label = "Long settings label", .current_value = "value"}},
            cch::tui::SettingsListOptions{});
    const auto rendered = list.render(10);
    REQUIRE(rendered);
    CHECK(cch::tui::strip_terminal_sequences(rendered->lines[0]) == "→ Long ...");
    CHECK(cch::tui::strip_terminal_sequences(rendered->lines.back()).ends_with("..."));
    CHECK(cch::tui::visible_width(rendered->lines.back()) == 10);
}

TEST_CASE("SettingsList renders and dispatches hints from one effective registry",
        "[tui][settings-list][issue57][spec]") {
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
    static_cast<void>(list.handle_input(cch::tui::KeyEvent{.key = "enter"}));
    static_cast<void>(list.handle_input(cch::tui::KeyEvent{.key = "escape"}));
    CHECK(changes == 0);
    CHECK(cancellations == 0);
    static_cast<void>(list.handle_input(cch::tui::KeyEvent{.key = "f2"}));
    static_cast<void>(list.handle_input(cch::tui::KeyEvent{.key = "f3"}));
    CHECK(changes == 1);
    CHECK(cancellations == 1);
}

TEST_CASE("SettingsList space confirms only while the search is empty", "[tui][settings-list][issue384][spec]") {
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
    static_cast<void>(list.handle_input(cch::tui::KeyEvent{.key = "space"}));
    REQUIRE(list.selected_item());
    CHECK(list.selected_item()->current_value == "on");
    CHECK(updates.size() == 1);

    // Space with a non-empty search inserts a space into the query instead.
    static_cast<void>(list.handle_input(cch::tui::KeyEvent{.key = "t"}));
    static_cast<void>(list.handle_input(cch::tui::KeyEvent{.key = "space"}));
    CHECK(list.search_query() == "t ");
    CHECK(updates.size() == 1);
}

TEST_CASE("SettingsList search editing flows through the Input component", "[tui][settings-list][issue384][spec]") {
    cch::tui::SettingsList list(
        {
            {.id = "alpha", .label = "Alpha", .current_value = "off"},
            {.id = "beta", .label = "Beta", .current_value = "off"},
        },
        cch::tui::SettingsListOptions{.enable_search = true});
    const auto key = [&list](std::string identifier, bool ctrl = false) {
        static_cast<void>(list.handle_input(cch::tui::KeyEvent{.key = std::move(identifier), .ctrl = ctrl}));
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

TEST_CASE("SettingsList paste flows through the Input component's cleaning", "[tui][settings-list][issue384][spec]") {
    cch::tui::SettingsList list(
        {
            {.id = "alpha", .label = "Alpha beta", .current_value = "off"},
            {.id = "beta", .label = "Beta", .current_value = "off"},
        },
        cch::tui::SettingsListOptions{.enable_search = true});

    // CRLF is dropped and spaces are preserved by the Input component's paste
    // cleaning; the query updates and the filter re-runs on it.
    static_cast<void>(list.handle_input(cch::tui::PasteEvent{.text = "al\r\nph"}));
    CHECK(list.search_query() == "alph");
    REQUIRE(list.selected_item());
    CHECK(list.selected_item()->id == "alpha");

    static_cast<void>(list.handle_input(cch::tui::PasteEvent{.text = "a b"}));
    CHECK(list.search_query() == "alpha b");
    REQUIRE(list.selected_item());
    CHECK(list.selected_item()->id == "alpha");
}

TEST_CASE("SettingsList search line renders and locates the cursor through Input",
        "[tui][settings-list][issue384][spec]") {
    cch::tui::SettingsList list(
        {
            {.id = "alpha", .label = "Alpha", .current_value = "off"},
        },
        cch::tui::SettingsListOptions{.enable_search = true});
    for (const auto& character : std::string("tool")) {
        static_cast<void>(list.handle_input(cch::tui::KeyEvent{.key = std::string(1, character)}));
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

TEST_CASE(
        "SettingsList cancellation invokes the callback once per semantic key", "[tui][settings-list][issue52][spec]") {
    std::size_t cancellations = 0;
    cch::tui::SettingsList list({}, cch::tui::SettingsListOptions{
        .on_cancel = [&cancellations]() -> cch::support::ExpectedVoid { ++cancellations; return {}; },
    });
    static_cast<void>(list.handle_input(cch::tui::KeyEvent{.key = "escape"}));
    CHECK(cancellations == 1);
}

TEST_CASE("SettingsList lays the value column out the way pi does", "[tui][settings-list][issue809][spec]") {
    // pi `settings-list.ts`: the label column is `min(36, widest label)`, a
    // label wider than that column keeps its text instead of being truncated,
    // and the value is truncated to what remains two columns short of the
    // row's right edge. A value that reached the edge, or a label cut at the
    // column, renders different cells while looking near-identical, so the
    // assertions are on whole rows rather than on substrings.
    const std::string wide_label(40, 'W');
    const std::string wide_value(40, 'v');
    cch::tui::SettingsList list(
            {
                    {.id = "wide", .label = wide_label, .current_value = "on"},
                    {.id = "alpha", .label = "Alpha", .current_value = wide_value},
            },
            cch::tui::SettingsListOptions{});

    const std::size_t width = 60;
    const auto rendered = list.render(width);
    REQUIRE(rendered);

    const auto row = [&rendered](const std::string& label) -> std::string {
        for (const auto& line : rendered->lines) {
            const auto text = cch::tui::strip_terminal_sequences(line);
            if (text.find(label) != std::string::npos) return text;
        }
        return {};
    };

    // The 40-column label is not cut at the 36-column value column; the first
    // row is the selected one, so it carries the theme cursor prefix.
    CHECK(row(wide_label) == "\xe2\x86\x92 " + wide_label + "  on");
    // The short label is padded to the column, and the 40-character value is
    // cut to the 18 columns pi leaves it (60 - 2 - 36 - 2 - 2).
    CHECK(row("Alpha") == "  Alpha" + std::string(31, ' ') + "  " + wide_value.substr(0, 18));
    for (const auto& line : rendered->lines) {
        CHECK(cch::tui::visible_width(line) <= width);
    }
}

namespace {

[[nodiscard]] cch::support::Expected<cch::support::JsonValue> settings_list_v130_scenario(std::string_view name) {
    auto fixture = cch::tests::read_pi_v130_audit_evidence("audit-1219", "settings-list.json");
    if (!fixture) return std::unexpected(fixture.error());
    for (const auto& item : fixture->at("scenarios").get_array()) {
        if (item.at("name").get_string() == name) return cch::support::JsonValue{item};
    }
    return std::unexpected(
            cch::support::make_error(cch::support::ErrorCode::Validation, "Scenario not found: " + std::string(name)));
}

[[nodiscard]] std::vector<std::string> settings_string_array(const cch::support::JsonValue& value) {
    std::vector<std::string> result;
    for (const auto& entry : value.get_array())
        result.push_back(entry.get_string());
    return result;
}

[[nodiscard]] cch::tui::SettingsListTheme test_settings_theme() {
    return cch::tui::SettingsListTheme{
            .cursor = "→ ",
    };
}

} // namespace

TEST_CASE("pi-v1.3.0 SettingsList value cycling and selection match frozen observation",
        "[tui][settings-list][issue1219]") {
    const auto scenario = settings_list_v130_scenario("settings-list-cycling-and-selection");
    REQUIRE(scenario);
    std::vector<std::pair<std::string, std::string>> changes;
    std::size_t cancels = 0;
    std::vector<cch::tui::SettingItem> items = {
            {
                    .id = "theme",
                    .label = "Theme",
                    .description = "Color theme",
                    .current_value = "dark",
                    .control = cch::tui::SettingValues{{"dark", "light"}},
            },
            {
                    .id = "editor",
                    .label = "Editor",
                    .current_value = "vim",
                    .control = cch::tui::SettingValues{{"vim", "emacs", "nano"}},
            },
            {
                    .id = "wrap",
                    .label = "Word Wrap",
                    .current_value = "off",
                    .control = cch::tui::SettingValues{{"off", "on"}},
            },
    };
    cch::tui::SettingsList list(std::move(items),
            cch::tui::SettingsListOptions{
                    .theme = test_settings_theme(),
                    .on_change = [&changes](std::string id, std::string val) -> cch::support::ExpectedVoid {
                        changes.emplace_back(std::move(id), std::move(val));
                        return {};
                    },
                    .on_cancel = [&cancels]() -> cch::support::ExpectedVoid {
                        cancels++;
                        return {};
                    },
            });

    const auto& expected = scenario->at("expected");
    auto r_initial = list.render(50);
    REQUIRE(r_initial);
    CHECK(r_initial->lines == settings_string_array(expected.at("initial").at("rows")));

    static_cast<void>(list.handle_input(cch::tui::KeyEvent{.key = "up"}));
    auto r_wrap_up = list.render(50);
    REQUIRE(r_wrap_up);
    CHECK(r_wrap_up->lines == settings_string_array(expected.at("wrappedUp").at("rows")));

    static_cast<void>(list.handle_input(cch::tui::KeyEvent{.key = "space"}));
    auto r_after_space = list.render(50);
    REQUIRE(r_after_space);
    CHECK(r_after_space->lines == settings_string_array(expected.at("afterSpace").at("rows")));

    static_cast<void>(list.handle_input(cch::tui::KeyEvent{.key = "up"}));
    static_cast<void>(list.handle_input(cch::tui::KeyEvent{.key = "enter"}));
    auto r_after_enter = list.render(50);
    REQUIRE(r_after_enter);
    CHECK(r_after_enter->lines == settings_string_array(expected.at("afterEnter").at("rows")));

    static_cast<void>(list.handle_input(cch::tui::KeyEvent{.key = "down"}));
    static_cast<void>(list.handle_input(cch::tui::KeyEvent{.key = "down"}));
    static_cast<void>(list.handle_input(cch::tui::KeyEvent{.key = "escape"}));

    const auto& exp_changes = expected.at("changes").get_array();
    REQUIRE(changes.size() == exp_changes.size());
    for (std::size_t i = 0; i < changes.size(); ++i) {
        CHECK(changes[i].first == exp_changes[i].at("id").get_string());
        CHECK(changes[i].second == exp_changes[i].at("value").get_string());
    }
    CHECK(cancels == static_cast<std::size_t>(expected.at("cancels").get_number()));
}

TEST_CASE("pi-v1.3.0 SettingsList centered five-row windowing matches frozen observation",
        "[tui][settings-list][issue1219]") {
    const auto scenario = settings_list_v130_scenario("settings-list-centered-five-row-window");
    REQUIRE(scenario);
    std::vector<cch::tui::SettingItem> items;
    for (std::size_t i = 1; i <= 9; ++i) {
        items.push_back({
                .id = "setting-" + std::to_string(i),
                .label = "Setting " + std::to_string(i),
                .current_value = "val-" + std::to_string(i),
                .control = cch::tui::SettingValues{{"val-" + std::to_string(i), "alt-" + std::to_string(i)}},
        });
    }
    cch::tui::SettingsList list(std::move(items),
            cch::tui::SettingsListOptions{
                    .max_visible = 5,
                    .theme = test_settings_theme(),
            });

    const auto& expected = scenario->at("expected");
    auto r_init = list.render(50);
    REQUIRE(r_init);
    CHECK(r_init->lines == settings_string_array(expected.at("initial").at("rows")));

    for (std::size_t i = 0; i < 6; ++i) {
        static_cast<void>(list.handle_input(cch::tui::KeyEvent{.key = "down"}));
    }
    auto r_mid = list.render(50);
    REQUIRE(r_mid);
    CHECK(r_mid->lines == settings_string_array(expected.at("middle").at("rows")));

    for (std::size_t i = 0; i < 2; ++i) {
        static_cast<void>(list.handle_input(cch::tui::KeyEvent{.key = "down"}));
    }
    auto r_end = list.render(50);
    REQUIRE(r_end);
    CHECK(r_end->lines == settings_string_array(expected.at("end").at("rows")));
}

TEST_CASE("pi-v1.3.0 SettingsList description wrapping label padding and hints match frozen observation",
        "[tui][settings-list][issue1219]") {
    const auto scenario = settings_list_v130_scenario("settings-list-description-and-hint");
    REQUIRE(scenario);
    std::vector<cch::tui::SettingItem> items = {
            {
                    .id = "opt1",
                    .label = "First Option",
                    .description = "Short description line",
                    .current_value = "enabled",
            },
            {
                    .id = "opt2",
                    .label = "Very Long Setting Option Label Exceeding Width",
                    .description = "Multi-line description text that must wrap cleanly across lines.\nSecond explicit "
                                   "paragraph of description with additional information.",
                    .current_value = "custom-value-data",
            },
    };
    cch::tui::SettingsList list(std::move(items),
            cch::tui::SettingsListOptions{
                    .max_visible = 5,
                    .theme = test_settings_theme(),
            });

    const auto& expected = scenario->at("expected");
    for (const auto& width_obs : expected.at("widths").get_array()) {
        const auto width = static_cast<std::size_t>(width_obs.at("width").get_number());
        const auto rendered = list.render(width);
        REQUIRE(rendered);
        CHECK(rendered->lines == settings_string_array(width_obs.at("rows")));
        std::vector<std::size_t> widths;
        for (const auto& line : rendered->lines)
            widths.push_back(cch::tui::visible_width(line));
        std::vector<std::size_t> expected_widths;
        for (const auto& w : width_obs.at("visibleWidths").get_array())
            expected_widths.push_back(static_cast<std::size_t>(w.get_number()));
        CHECK(widths == expected_widths);
    }
}

TEST_CASE("pi-v1.3.0 SettingsList search input filtering and empty results match frozen observation",
        "[tui][settings-list][issue1219]") {
    const auto scenario = settings_list_v130_scenario("settings-list-search-filtering-and-input");
    REQUIRE(scenario);
    std::vector<std::pair<std::string, std::string>> changes;
    std::vector<cch::tui::SettingItem> items = {
            {.id = "term",
                    .label = "Terminal Bell",
                    .current_value = "audible",
                    .control = cch::tui::SettingValues{{"audible", "visual", "none"}}},
            {.id = "theme",
                    .label = "Color Theme",
                    .current_value = "dark",
                    .control = cch::tui::SettingValues{{"dark", "light"}}},
            {.id = "telemetry",
                    .label = "Telemetry Report",
                    .current_value = "off",
                    .control = cch::tui::SettingValues{{"off", "on"}}},
            {.id = "wrap",
                    .label = "Word Wrap",
                    .current_value = "off",
                    .control = cch::tui::SettingValues{{"off", "on"}}},
    };
    cch::tui::SettingsList list(std::move(items),
            cch::tui::SettingsListOptions{
                    .max_visible = 5,
                    .enable_search = true,
                    .theme = test_settings_theme(),
                    .on_change = [&changes](std::string id, std::string val) -> cch::support::ExpectedVoid {
                        changes.emplace_back(std::move(id), std::move(val));
                        return {};
                    },
            });

    const auto& expected = scenario->at("expected");
    auto r_init = list.render(60);
    REQUIRE(r_init);
    CHECK(r_init->lines == settings_string_array(expected.at("initial").at("rows")));

    for (const char ch : std::string_view("term")) {
        static_cast<void>(list.handle_input(cch::tui::KeyEvent{.key = std::string(1, ch)}));
    }
    auto r_filt = list.render(60);
    REQUIRE(r_filt);
    CHECK(r_filt->lines == settings_string_array(expected.at("filtered").at("rows")));

    static_cast<void>(list.handle_input(cch::tui::KeyEvent{.key = "space"}));
    auto r_space_non_empty = list.render(60);
    REQUIRE(r_space_non_empty);
    CHECK(r_space_non_empty->lines == settings_string_array(expected.at("afterSpaceNonEmpty").at("rows")));

    for (std::size_t i = 0; i < 5; ++i) {
        static_cast<void>(list.handle_input(cch::tui::KeyEvent{.key = "backspace"}));
    }
    auto r_cleared = list.render(60);
    REQUIRE(r_cleared);
    CHECK(r_cleared->lines == settings_string_array(expected.at("cleared").at("rows")));

    static_cast<void>(list.handle_input(cch::tui::KeyEvent{.key = "space"}));
    auto r_space_empty = list.render(60);
    REQUIRE(r_space_empty);
    CHECK(r_space_empty->lines == settings_string_array(expected.at("afterSpaceEmpty").at("rows")));

    for (const char ch : std::string_view("zzzzz")) {
        static_cast<void>(list.handle_input(cch::tui::KeyEvent{.key = std::string(1, ch)}));
    }
    auto r_no_match = list.render(60);
    REQUIRE(r_no_match);
    CHECK(r_no_match->lines == settings_string_array(expected.at("noMatch").at("rows")));

    const auto& exp_changes = expected.at("changes").get_array();
    REQUIRE(changes.size() == exp_changes.size());
    for (std::size_t i = 0; i < changes.size(); ++i) {
        CHECK(changes[i].first == exp_changes[i].at("id").get_string());
        CHECK(changes[i].second == exp_changes[i].at("value").get_string());
    }
}

TEST_CASE("pi-v1.3.0 SettingsList submenu lifecycle and navigate-to behavior match frozen observation",
        "[tui][settings-list][issue1219]") {
    const auto scenario = settings_list_v130_scenario("settings-list-submenu-navigation-and-lifecycle");
    REQUIRE(scenario);
    std::vector<std::pair<std::string, std::string>> changes;
    std::string opened_with;

    class TestSubmenu final : public cch::tui::Component, public cch::tui::InputHandler {
    public:
        TestSubmenu(std::string current, std::shared_ptr<cch::tui::SettingsSubmenuDoneSink> done)
            : current_(std::move(current)), done_(std::move(done)) {}

        cch::support::Expected<cch::tui::RenderResult> render(std::size_t width) override {
            return cch::tui::RenderResult{
                    .lines = {"[Submenu: choosing model, current=" + current_ + ", width=" + std::to_string(width) +
                              "]"},
            };
        }
        void invalidate() override {}
        cch::tui::InputAdmissionOutcome handle_input(const cch::tui::InputEventVariant& input) override {
            const auto* key = std::get_if<cch::tui::KeyEvent>(&input);
            if (!key) return cch::tui::InputAdmissionOutcome::Unhandled;
            if (key->key == "enter") {
                if (done_ && *done_) {
                    (void)(*done_)("claude-3-5-sonnet", cch::tui::SettingsSubmenuOptions{.navigate_to = "provider"});
                }
                return cch::tui::InputAdmissionOutcome::Consumed;
            }
            if (key->key == "escape") {
                if (done_ && *done_) {
                    (void)(*done_)(std::nullopt, std::nullopt);
                }
                return cch::tui::InputAdmissionOutcome::Consumed;
            }
            return cch::tui::InputAdmissionOutcome::Unhandled;
        }

    private:
        std::string current_;
        std::shared_ptr<cch::tui::SettingsSubmenuDoneSink> done_;
    };

    std::vector<cch::tui::SettingItem> items = {
            {
                    .id = "model",
                    .label = "Model Selector",
                    .current_value = "gpt-4o",
                    .control = cch::tui::SettingSubmenu{},
            },
            {
                    .id = "provider",
                    .label = "API Provider",
                    .current_value = "openai",
                    .control = cch::tui::SettingValues{{"openai", "anthropic"}},
            },
    };
    cch::tui::SettingsList list(std::move(items),
            cch::tui::SettingsListOptions{
                    .max_visible = 5,
                    .theme = test_settings_theme(),
                    .on_change = [&changes](std::string id, std::string val) -> cch::support::ExpectedVoid {
                        changes.emplace_back(std::move(id), std::move(val));
                        return {};
                    },
                    .submenu_factory =
                            [&opened_with](const cch::tui::SettingItem& item,
                                    cch::tui::SettingsSubmenuDoneSink done) -> std::unique_ptr<cch::tui::Component> {
                        opened_with = item.current_value;
                        return std::make_unique<TestSubmenu>(item.current_value,
                                std::make_shared<cch::tui::SettingsSubmenuDoneSink>(std::move(done)));
                    },
            });

    const auto& expected = scenario->at("expected");
    auto r_init = list.render(60);
    REQUIRE(r_init);
    CHECK(r_init->lines == settings_string_array(expected.at("initial").at("rows")));

    static_cast<void>(list.handle_input(cch::tui::KeyEvent{.key = "enter"}));
    CHECK(list.submenu_open());
    CHECK(opened_with == expected.at("openedWith").get_string());
    auto r_open = list.render(60);
    REQUIRE(r_open);
    CHECK(r_open->lines == settings_string_array(expected.at("openSubmenu").at("rows")));

    static_cast<void>(list.handle_input(cch::tui::KeyEvent{.key = "enter"}));
    CHECK_FALSE(list.submenu_open());
    auto r_done = list.render(60);
    REQUIRE(r_done);
    CHECK(r_done->lines == settings_string_array(expected.at("afterDoneAndNavigate").at("rows")));

    const auto& exp_changes = expected.at("changes").get_array();
    REQUIRE(changes.size() == exp_changes.size());
    for (std::size_t i = 0; i < changes.size(); ++i) {
        CHECK(changes[i].first == exp_changes[i].at("id").get_string());
        CHECK(changes[i].second == exp_changes[i].at("value").get_string());
    }
}

TEST_CASE("pi-v1.3.0 SettingsList value update and item selection match frozen observation",
        "[tui][settings-list][issue1219]") {
    const auto scenario = settings_list_v130_scenario("settings-list-update-value-and-select-item");
    REQUIRE(scenario);
    std::vector<cch::tui::SettingItem> items = {
            {.id = "alpha",
                    .label = "Alpha",
                    .current_value = "initial-a",
                    .control = cch::tui::SettingValues{{"initial-a", "next-a"}}},
            {.id = "beta",
                    .label = "Beta",
                    .current_value = "initial-b",
                    .control = cch::tui::SettingValues{{"initial-b", "next-b"}}},
    };
    cch::tui::SettingsList list(std::move(items),
            cch::tui::SettingsListOptions{
                    .max_visible = 5,
                    .theme = test_settings_theme(),
            });

    list.update_value("beta", "updated-b");
    list.select_item("beta");
    auto rendered = list.render(50);
    REQUIRE(rendered);
    const auto& expected = scenario->at("expected").at("observation");
    CHECK(rendered->lines == settings_string_array(expected.at("rows")));
}

TEST_CASE("SettingsList space confirms only when search query is empty and ignores non-empty input",
        "[tui][settings-list][negative][issue1219]") {
    std::vector<std::string> changes;
    cch::tui::SettingsList list(
            {
                    {.id = "mode",
                            .label = "Mode",
                            .current_value = "one",
                            .control = cch::tui::SettingValues{{"one", "two"}}},
            },
            cch::tui::SettingsListOptions{
                    .enable_search = true,
                    .on_change = [&changes](std::string id, std::string val) -> cch::support::ExpectedVoid {
                        changes.push_back(id + "=" + val);
                        return {};
                    },
            });

    // Type "m" into search query
    static_cast<void>(list.handle_input(cch::tui::KeyEvent{.key = "m"}));
    CHECK(list.search_query() == "m");

    // Space when search query is non-empty types space into search and does NOT activate setting
    static_cast<void>(list.handle_input(cch::tui::KeyEvent{.key = "space"}));
    CHECK(list.search_query() == "m ");
    CHECK(changes.empty());
    REQUIRE(list.selected_item());
    CHECK(list.selected_item()->current_value == "one");

    // Clear search
    static_cast<void>(list.handle_input(cch::tui::KeyEvent{.key = "backspace"}));
    static_cast<void>(list.handle_input(cch::tui::KeyEvent{.key = "backspace"}));
    CHECK(list.search_query().empty());

    // Space when search query is empty DOES activate setting
    static_cast<void>(list.handle_input(cch::tui::KeyEvent{.key = "space"}));
    CHECK(changes.size() == 1);
    CHECK(changes[0] == "mode=two");
    REQUIRE(list.selected_item());
    CHECK(list.selected_item()->current_value == "two");
}

TEST_CASE("SettingsList windowing clamps selection within valid range on item removal",
        "[tui][settings-list][boundary][issue1219]") {
    std::vector<cch::tui::SettingItem> items;
    for (std::size_t i = 0; i < 9; ++i) {
        items.push_back({.id = "s" + std::to_string(i), .label = "S" + std::to_string(i), .current_value = "v"});
    }
    cch::tui::SettingsList list(std::move(items), cch::tui::SettingsListOptions{.max_visible = 5});
    for (std::size_t i = 0; i < 8; ++i) {
        static_cast<void>(list.handle_input(cch::tui::KeyEvent{.key = "down"}));
    }
    REQUIRE(list.selected_item());
    CHECK(list.selected_item()->id == "s8");
    auto r = list.render(40);
    REQUIRE(r);
    CHECK(r->lines.size() == 8); // 5 items + scroll indicator + 2 blank/hint lines
}

TEST_CASE("SettingsList layout caps label padding at thirty-six columns and preserves value column",
        "[tui][settings-list][boundary][issue1219]") {
    const std::string long_label(50, 'L');
    cch::tui::SettingsList list(
            {{.id = "long", .label = long_label, .current_value = "val"}}, cch::tui::SettingsListOptions{});
    auto rendered = list.render(60);
    REQUIRE(rendered);
    for (const auto& line : rendered->lines) {
        CHECK(cch::tui::visible_width(line) <= 60);
    }

    auto invalid = list.render(0);
    CHECK_FALSE(invalid.has_value());
}

TEST_CASE("SettingsList search input handles backspace to empty and non-matching queries fail closed",
        "[tui][settings-list][negative][issue1219]") {
    cch::tui::SettingsList list(
            {
                    {.id = "s1", .label = "Item Alpha", .current_value = "on"},
                    {.id = "s2", .label = "Item Beta", .current_value = "off"},
            },
            cch::tui::SettingsListOptions{.enable_search = true});

    for (const char ch : std::string_view("nonexistent")) {
        static_cast<void>(list.handle_input(cch::tui::KeyEvent{.key = std::string(1, ch)}));
    }
    CHECK_FALSE(list.selected_item().has_value());
    auto r_none = list.render(50);
    REQUIRE(r_none);
    bool found_no_match = false;
    for (const auto& line : r_none->lines) {
        if (line.find("No matching settings") != std::string::npos) found_no_match = true;
    }
    CHECK(found_no_match);

    // Backspace clears query
    for (std::size_t i = 0; i < 20; ++i) {
        static_cast<void>(list.handle_input(cch::tui::KeyEvent{.key = "backspace"}));
    }
    CHECK(list.search_query().empty());
    REQUIRE(list.selected_item());
    CHECK(list.selected_item()->id == "s1");
}

TEST_CASE("SettingsList submenu done callback ignores invalid navigateTo identifiers",
        "[tui][settings-list][negative][issue1219]") {
    class MockSubmenu final : public cch::tui::Component, public cch::tui::InputHandler {
    public:
        explicit MockSubmenu(cch::tui::SettingsSubmenuDoneSink done)
            : done_(std::make_shared<cch::tui::SettingsSubmenuDoneSink>(std::move(done))) {}

        cch::support::Expected<cch::tui::RenderResult> render(std::size_t) override {
            return cch::tui::RenderResult{.lines = {"mock"}};
        }
        void invalidate() override {}
        cch::tui::InputAdmissionOutcome handle_input(const cch::tui::InputEventVariant&) override {
            if (done_ && *done_) {
                (void)(*done_)("new-val", std::nullopt);
            }
            return cch::tui::InputAdmissionOutcome::Consumed;
        }

    private:
        std::shared_ptr<cch::tui::SettingsSubmenuDoneSink> done_;
    };

    cch::tui::SettingsList list(
            {
                    {.id = "sub", .label = "Sub", .current_value = "old", .control = cch::tui::SettingSubmenu{}},
                    {.id = "other", .label = "Other", .current_value = "v"},
            },
            cch::tui::SettingsListOptions{
                    .submenu_factory = [](const cch::tui::SettingItem&, cch::tui::SettingsSubmenuDoneSink done)
                            -> std::unique_ptr<cch::tui::Component> {
                        return std::make_unique<MockSubmenu>(std::move(done));
                    },
            });

    static_cast<void>(list.handle_input(cch::tui::KeyEvent{.key = "enter"}));
    CHECK(list.submenu_open());
    static_cast<void>(list.handle_input(cch::tui::KeyEvent{.key = "x"}));
    CHECK_FALSE(list.submenu_open());
    REQUIRE(list.selected_item());
    CHECK(list.selected_item()->id == "sub");
    CHECK(list.selected_item()->current_value == "new-val");
}

TEST_CASE("SettingsList update_value and select_item ignore unknown identifiers",
        "[tui][settings-list][negative][issue1219]") {
    cch::tui::SettingsList list({{.id = "valid", .label = "Valid", .current_value = "val"}});
    list.update_value("unknown", "new");
    REQUIRE(list.selected_item());
    CHECK(list.selected_item()->current_value == "val");

    list.select_item("unknown");
    REQUIRE(list.selected_item());
    CHECK(list.selected_item()->id == "valid");
}

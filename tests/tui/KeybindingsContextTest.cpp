#include <cch/tui/Editor.hpp>
#include <cch/tui/Input.hpp>
#include <cch/tui/Keybindings.hpp>
#include <cch/tui/SelectList.hpp>
#include <cch/tui/SettingsList.hpp>
#include "coding_agent/tui/SharedKeybindings.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

using namespace cch;

namespace {

[[nodiscard]] std::shared_ptr<const tui::KeybindingRegistry> make_custom_registry(
        std::vector<tui::KeybindingOverride> overrides) {
    auto resolution = tui::resolve_keybindings(tui::KeybindingResolutionRequest{
            .definitions = tui::builtin_tui_keybinding_definitions(),
            .overrides = std::move(overrides),
    });
    REQUIRE(resolution);
    return resolution->registry;
}

} // namespace

TEST_CASE("KeybindingsContext isolates independent contexts and updates live across dispatch",
        "[tui][keybindings][issue959][spec]") {
    auto context1 = std::make_shared<tui::KeybindingsContext>();
    auto context2 = std::make_shared<tui::KeybindingsContext>();

    // Initial state: default mappings
    CHECK(context1->key_text("tui.editor.deleteToLineStart") == "ctrl+u");
    CHECK(context2->key_text("tui.editor.deleteToLineStart") == "ctrl+u");

    // Construct an editor wired to context1
    tui::Editor editor(tui::EditorOptions{.keybinding_context = context1});
    editor.set_text("hello world");
    CHECK(editor.expanded_text() == "hello world");

    // Press ctrl+u: deletes to line start under default bindings
    auto outcome = editor.handle_input(tui::KeyEvent{.key = "u", .ctrl = true});
    CHECK(outcome == tui::InputAdmissionOutcome::Consumed);
    CHECK(editor.expanded_text().empty());

    // Replace context1's keybindings: remap deleteToLineStart to ctrl+x
    auto custom_reg = make_custom_registry({
            tui::KeybindingOverride{.id = "tui.editor.deleteToLineStart", .keys = {"ctrl+x"}},
    });
    context1->replace(custom_reg);

    // context1 reflects change immediately
    CHECK(context1->key_text("tui.editor.deleteToLineStart") == "ctrl+x");
    CHECK(context1->matches(tui::KeyEvent{.key = "x", .ctrl = true}, "tui.editor.deleteToLineStart"));
    CHECK_FALSE(context1->matches(tui::KeyEvent{.key = "u", .ctrl = true}, "tui.editor.deleteToLineStart"));

    // context2 remains isolated and unchanged
    CHECK(context2->key_text("tui.editor.deleteToLineStart") == "ctrl+u");
    CHECK(context2->matches(tui::KeyEvent{.key = "u", .ctrl = true}, "tui.editor.deleteToLineStart"));

    // Editor wired to context1 now dispatches with new binding without re-creation
    editor.set_text("new text");
    CHECK(editor.expanded_text() == "new text");

    // Old key (ctrl+u) no longer deletes line
    outcome = editor.handle_input(tui::KeyEvent{.key = "u", .ctrl = true});
    CHECK(outcome == tui::InputAdmissionOutcome::Unhandled);
    CHECK(editor.expanded_text() == "new text");

    // New key (ctrl+x) deletes line
    outcome = editor.handle_input(tui::KeyEvent{.key = "x", .ctrl = true});
    CHECK(outcome == tui::InputAdmissionOutcome::Consumed);
    CHECK(editor.expanded_text().empty());
}

TEST_CASE("Input and SelectList and SettingsList adapt live to KeybindingsContext replacement",
        "[tui][keybindings][issue959][spec]") {
    SECTION("Input submits on updated keybinding") {
        auto context = std::make_shared<tui::KeybindingsContext>();
        std::vector<std::string> submitted;
        tui::Input input(tui::InputOptions{.keybinding_context = context},
                [&submitted](std::string val) -> support::ExpectedVoid {
                    submitted.push_back(std::move(val));
                    return {};
                });
        input.set_value("test query");

        // Remap submit from enter to ctrl+s
        auto custom_reg = make_custom_registry({
                tui::KeybindingOverride{.id = "tui.input.submit", .keys = {"ctrl+s"}},
        });
        context->replace(custom_reg);

        // Enter no longer submits
        auto outcome = input.handle_input(tui::KeyEvent{.key = "enter"});
        CHECK(outcome == tui::InputAdmissionOutcome::Unhandled);
        CHECK(submitted.empty());

        // Ctrl+s submits
        outcome = input.handle_input(tui::KeyEvent{.key = "s", .ctrl = true});
        CHECK(outcome == tui::InputAdmissionOutcome::Consumed);
        REQUIRE(submitted.size() == 1);
        CHECK(submitted[0] == "test query");
    }

    SECTION("SelectList cancels on updated keybinding") {
        auto context = std::make_shared<tui::KeybindingsContext>();
        bool cancelled = false;
        std::vector<tui::SelectItem> items{
                tui::SelectItem{.value = "1", .label = "First"},
                tui::SelectItem{.value = "2", .label = "Second"},
        };
        tui::SelectList list(items,
                tui::SelectListOptions{
                        .on_cancel = [&cancelled]() -> support::ExpectedVoid {
                            cancelled = true;
                            return {};
                        },
                        .keybinding_context = context,
                });

        // Remap cancel to ctrl+q
        auto custom_reg = make_custom_registry({
                tui::KeybindingOverride{.id = "tui.select.cancel", .keys = {"ctrl+q"}},
        });
        context->replace(custom_reg);

        // Escape no longer cancels
        auto outcome = list.handle_input(tui::KeyEvent{.key = "escape"});
        CHECK(outcome == tui::InputAdmissionOutcome::Unhandled);
        CHECK_FALSE(cancelled);

        // Ctrl+q cancels
        outcome = list.handle_input(tui::KeyEvent{.key = "q", .ctrl = true});
        CHECK(outcome == tui::InputAdmissionOutcome::Consumed);
        CHECK(cancelled);
    }

    SECTION("SettingsList hint reflects runtime keybinding replacement immediately") {
        auto context = std::make_shared<tui::KeybindingsContext>();
        std::vector<tui::SettingItem> items{
                tui::SettingItem{.id = "theme", .label = "Theme", .current_value = "dark"},
        };
        tui::SettingsList list(items, tui::SettingsListOptions{.keybinding_context = context});

        // Initial render shows default "escape/ctrl+c to cancel"
        auto rendered = list.render(80);
        REQUIRE(rendered);
        bool found_default_hint = false;
        for (const auto& line : rendered->lines) {
            if (line.find("escape/ctrl+c to cancel") != std::string::npos) {
                found_default_hint = true;
                break;
            }
        }
        CHECK(found_default_hint);

        // Remap cancel to q
        auto custom_reg = make_custom_registry({
                tui::KeybindingOverride{.id = "tui.select.cancel", .keys = {"q"}},
        });
        context->replace(custom_reg);

        // Subsequent render immediately shows "q to cancel"
        rendered = list.render(80);
        REQUIRE(rendered);
        bool found_updated_hint = false;
        for (const auto& line : rendered->lines) {
            if (line.find("q to cancel") != std::string::npos) {
                found_updated_hint = true;
                break;
            }
        }
        CHECK(found_updated_hint);
    }
}

TEST_CASE("alt_screen_keybinding_definitions matches frozen pi v1.0.4 defaults", "[tui][keybindings][issue959][spec]") {
    const auto defs = tui::alt_screen_keybinding_definitions();
    REQUIRE(defs.size() == 14);

    const auto find_def = [&](std::string_view id) -> const tui::KeybindingDefinition* {
        const auto it = std::find_if(defs.begin(), defs.end(), [id](const auto& d) { return d.id == id; });
        return it == defs.end() ? nullptr : &*it;
    };

    const auto* page_up = find_def("tui.altScreen.pageUp");
    REQUIRE(page_up);
    CHECK(page_up->default_keys == std::vector<std::string>{"pageUp"});

    const auto* page_down = find_def("tui.altScreen.pageDown");
    REQUIRE(page_down);
    CHECK(page_down->default_keys == std::vector<std::string>{"pageDown"});

    const auto* half_page_up = find_def("tui.altScreen.halfPageUp");
    REQUIRE(half_page_up);
    CHECK(half_page_up->default_keys.empty());

    const auto* half_page_down = find_def("tui.altScreen.halfPageDown");
    REQUIRE(half_page_down);
    CHECK(half_page_down->default_keys.empty());

    const auto* line_up = find_def("tui.altScreen.lineUp");
    REQUIRE(line_up);
    CHECK(line_up->default_keys.empty());

    const auto* line_down = find_def("tui.altScreen.lineDown");
    REQUIRE(line_down);
    CHECK(line_down->default_keys.empty());

    const auto* prev_prompt = find_def("tui.altScreen.previousPrompt");
    REQUIRE(prev_prompt);
    CHECK(prev_prompt->default_keys == std::vector<std::string>{"ctrl+shift+up", "ctrl+up"});

    const auto* next_prompt = find_def("tui.altScreen.nextPrompt");
    REQUIRE(next_prompt);
    CHECK(next_prompt->default_keys == std::vector<std::string>{"ctrl+shift+down", "ctrl+down"});

    const auto* search = find_def("tui.altScreen.search");
    REQUIRE(search);
    CHECK(search->default_keys == std::vector<std::string>{"ctrl+shift+f"});

    const auto* search_next = find_def("tui.altScreen.searchNext");
    REQUIRE(search_next);
    CHECK(search_next->default_keys == std::vector<std::string>{"enter", "ctrl+g"});

    const auto* search_prev = find_def("tui.altScreen.searchPrevious");
    REQUIRE(search_prev);
    CHECK(search_prev->default_keys == std::vector<std::string>{"shift+enter", "ctrl+shift+g"});

    const auto* search_close = find_def("tui.altScreen.searchClose");
    REQUIRE(search_close);
    CHECK(search_close->default_keys == std::vector<std::string>{"escape"});

    const auto* top = find_def("tui.altScreen.top");
    REQUIRE(top);
    CHECK(top->default_keys == std::vector<std::string>{"ctrl+home"});

    const auto* bottom = find_def("tui.altScreen.bottom");
    REQUIRE(bottom);
    CHECK(bottom->default_keys == std::vector<std::string>{"ctrl+end"});
}

TEST_CASE("SharedKeybindings synchronizes context and registry", "[coding_agent][tui][keybindings][issue959][spec]") {
    coding_agent::tui::SharedKeybindings shared;
    REQUIRE(shared);
    REQUIRE(shared.context());

    // Both registry() and context()->snapshot() match initial defaults
    CHECK(shared.registry().key_text("tui.input.submit") == "enter");
    CHECK(shared.context()->key_text("tui.input.submit") == "enter");

    // Replace with custom
    auto custom = make_custom_registry({
            tui::KeybindingOverride{.id = "tui.input.submit", .keys = {"ctrl+m"}},
    });
    shared.replace(custom);

    // Both reflect the replace immediately
    CHECK(shared.registry().key_text("tui.input.submit") == "ctrl+m");
    CHECK(shared.context()->key_text("tui.input.submit") == "ctrl+m");
    CHECK(shared.snapshot()->key_text("tui.input.submit") == "ctrl+m");
}

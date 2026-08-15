#include <cch/tui/Keybindings.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <string>
#include <vector>

using namespace cch;

TEST_CASE("Keybinding resolution replaces defaults with canonical user alternatives", "[tui][keybindings][issue57]") {
    tui::KeybindingResolutionRequest request;
    request.definitions = tui::builtin_tui_keybinding_definitions();
    request.overrides = {
        {.id = "tui.input.submit", .keys = {"ctrl+enter", "return", "enter"}},
        {.id = "tui.input.newLine", .keys = {}},
    };

    const auto resolved = tui::resolve_keybindings(std::move(request));

    REQUIRE(resolved);
    CHECK((resolved->registry->keys("tui.input.submit") ==
        std::vector<std::string>{"ctrl+enter", "enter"}));
    CHECK(resolved->registry->keys("tui.input.newLine").empty());
    CHECK(resolved->registry->matches(
        tui::KeyEvent{.key = "enter", .ctrl = true},
        "tui.input.submit"));
    CHECK_FALSE(resolved->registry->matches(
        tui::KeyEvent{.key = "enter", .shift = true},
        "tui.input.newLine"));
}

TEST_CASE(
    "User key conflicts preserve context defaults and resolve by candidate order",
    "[tui][keybindings][issue57]") {
    tui::KeybindingResolutionRequest request;
    request.definitions = tui::builtin_tui_keybinding_definitions();
    request.overrides = {
        {.id = "tui.editor.cursorUp", .keys = {"down"}},
        {.id = "tui.select.up", .keys = {"down"}},
    };

    const auto resolved = tui::resolve_keybindings(std::move(request));

    REQUIRE(resolved);
    CHECK(resolved->registry->keys("tui.editor.cursorDown") == std::vector<std::string>{"down"});
    CHECK(resolved->registry->keys("tui.select.down") == std::vector<std::string>{"down"});
    REQUIRE(resolved->issues.size() == 1);
    CHECK(resolved->issues.front().code == "conflicting_user_key");
    constexpr std::array<std::string_view, 2> kEditorFirst{
        "tui.editor.cursorUp",
        "tui.select.up",
    };
    constexpr std::array<std::string_view, 2> kSelectFirst{
        "tui.select.up",
        "tui.editor.cursorUp",
    };
    const tui::KeyEvent down{.key = "down"};
    REQUIRE(resolved->registry->first_match(down, kEditorFirst));
    CHECK(*resolved->registry->first_match(down, kEditorFirst) == "tui.editor.cursorUp");
    REQUIRE(resolved->registry->first_match(down, kSelectFirst));
    CHECK(*resolved->registry->first_match(down, kSelectFirst) == "tui.select.up");
}

TEST_CASE(
    "Invalid overrides retain defaults and key display follows the registry",
    "[tui][keybindings][issue57]") {
    tui::KeybindingResolutionRequest request;
    request.definitions = tui::builtin_tui_keybinding_definitions();
    request.overrides = {{.id = "tui.editor.cursorWordLeft", .keys = {"super+left"}}};

    const auto resolved = tui::resolve_keybindings(std::move(request));

    REQUIRE(resolved);
    REQUIRE(resolved->issues.size() == 1);
    CHECK(resolved->issues.front().code == "invalid_key");
    CHECK((resolved->registry->keys("tui.editor.cursorWordLeft") ==
        std::vector<std::string>{"alt+left", "ctrl+left", "alt+b"}));
    CHECK(resolved->registry->key_text("tui.editor.cursorWordLeft") ==
        "alt+left/ctrl+left/alt+b");
}

TEST_CASE(
    "Builtin table is exactly the frozen pi default table at 83114817",
    "[tui][keybindings][issue382]") {
    // Re-verified action-for-action against pi 83114817
    // packages/tui/src/keybindings.ts TUI_KEYBINDINGS: 21 tui.editor.*,
    // 3 tui.input.*, and 6 tui.select.* with pi's default keys and
    // descriptions.
    struct FrozenEntry {
        std::string id;
        std::vector<std::string> keys;
        std::string description;
    };
    const std::vector<FrozenEntry> kFrozenTable{
        {"tui.editor.cursorUp", {"up"}, "Move cursor up"},
        {"tui.editor.cursorDown", {"down"}, "Move cursor down"},
        {"tui.editor.cursorLeft", {"left", "ctrl+b"}, "Move cursor left"},
        {"tui.editor.cursorRight", {"right", "ctrl+f"}, "Move cursor right"},
        {"tui.editor.cursorWordLeft", {"alt+left", "ctrl+left", "alt+b"}, "Move cursor word left"},
        {"tui.editor.cursorWordRight", {"alt+right", "ctrl+right", "alt+f"}, "Move cursor word right"},
        {"tui.editor.cursorLineStart", {"home", "ctrl+a"}, "Move to line start"},
        {"tui.editor.cursorLineEnd", {"end", "ctrl+e"}, "Move to line end"},
        {"tui.editor.jumpForward", {"ctrl+]"}, "Jump forward to character"},
        {"tui.editor.jumpBackward", {"ctrl+alt+]"}, "Jump backward to character"},
        {"tui.editor.pageUp", {"pageUp"}, "Page up"},
        {"tui.editor.pageDown", {"pageDown"}, "Page down"},
        {"tui.editor.deleteCharBackward", {"backspace"}, "Delete character backward"},
        {"tui.editor.deleteCharForward", {"delete", "ctrl+d"}, "Delete character forward"},
        {"tui.editor.deleteWordBackward", {"ctrl+w", "alt+backspace"}, "Delete word backward"},
        {"tui.editor.deleteWordForward", {"alt+d", "alt+delete"}, "Delete word forward"},
        {"tui.editor.deleteToLineStart", {"ctrl+u"}, "Delete to line start"},
        {"tui.editor.deleteToLineEnd", {"ctrl+k"}, "Delete to line end"},
        {"tui.editor.yank", {"ctrl+y"}, "Yank"},
        {"tui.editor.yankPop", {"alt+y"}, "Yank pop"},
        {"tui.editor.undo", {"ctrl+-"}, "Undo"},
        {"tui.input.newLine", {"shift+enter", "ctrl+j"}, "Insert newline"},
        {"tui.input.submit", {"enter"}, "Submit input"},
        {"tui.input.tab", {"tab"}, "Tab / autocomplete"},
        {"tui.select.up", {"up"}, "Move selection up"},
        {"tui.select.down", {"down"}, "Move selection down"},
        {"tui.select.pageUp", {"pageUp"}, "Selection page up"},
        {"tui.select.pageDown", {"pageDown"}, "Selection page down"},
        {"tui.select.confirm", {"enter"}, "Confirm selection"},
        {"tui.select.cancel", {"escape", "ctrl+c"}, "Cancel selection"},
    };

    const auto definitions = tui::builtin_tui_keybinding_definitions();
    REQUIRE(definitions.size() == kFrozenTable.size());
    for (std::size_t index = 0; index < definitions.size(); ++index) {
        CHECK(definitions[index].id == kFrozenTable[index].id);
        CHECK(definitions[index].default_keys == kFrozenTable[index].keys);
        CHECK(definitions[index].description == kFrozenTable[index].description);
    }

    std::size_t editor = 0;
    std::size_t input = 0;
    std::size_t select = 0;
    for (const auto& definition : definitions) {
        if (definition.id.starts_with("tui.editor.")) ++editor;
        if (definition.id.starts_with("tui.input.")) ++input;
        if (definition.id.starts_with("tui.select.")) ++select;
    }
    CHECK(editor == 21);
    CHECK(input == 3);
    CHECK(select == 6);

    const auto registry = tui::default_tui_keybindings();
    CHECK(registry->entries().size() == 30);
    for (const auto& entry : kFrozenTable) {
        CHECK(registry->keys(entry.id) == entry.keys);
    }
}

TEST_CASE(
    "Known-but-unassembled IDs are recognized and never become no-op bindings",
    "[tui][keybindings][issue382]") {
    constexpr std::array<std::string_view, 7> kUnassembled{
        "tui.input.copy",
        "tui.altScreen.pageUp",
        "tui.altScreen.pageDown",
        "tui.altScreen.previousPrompt",
        "tui.altScreen.nextPrompt",
        "tui.altScreen.top",
        "tui.altScreen.bottom",
    };
    for (const auto id : kUnassembled) {
        CHECK(tui::is_known_unassembled_tui_keybinding(id));
    }
    CHECK_FALSE(tui::is_known_unassembled_tui_keybinding("tui.input.submit"));
    CHECK_FALSE(tui::is_known_unassembled_tui_keybinding("tui.editor.cursorUp"));
    CHECK_FALSE(tui::is_known_unassembled_tui_keybinding("tui.altScreen.zoom"));
    CHECK_FALSE(tui::is_known_unassembled_tui_keybinding("future.action"));

    const auto definitions = tui::builtin_tui_keybinding_definitions();
    for (const auto id : kUnassembled) {
        const auto found = std::find_if(
            definitions.begin(),
            definitions.end(),
            [id](const auto& definition) { return definition.id == id; });
        CHECK(found == definitions.end());
    }
    const auto registry = tui::default_tui_keybindings();
    for (const auto id : kUnassembled) {
        CHECK(registry->find(id) == nullptr);
        CHECK(registry->keys(id).empty());
        CHECK_FALSE(registry->matches(tui::KeyEvent{.key = "c", .ctrl = true}, id));
    }
}

TEST_CASE(
    "Known-but-unassembled overrides diagnose as unavailable, unknown ids stay unknown",
    "[tui][keybindings][issue382]") {
    tui::KeybindingResolutionRequest request;
    request.definitions = tui::builtin_tui_keybinding_definitions();
    request.overrides = {
        {.id = "tui.input.copy", .keys = {"ctrl+c"}},
        {.id = "tui.altScreen.previousPrompt", .keys = {"ctrl+shift+up"}},
        {.id = "future.action", .keys = {"f1"}},
    };

    const auto resolved = tui::resolve_keybindings(std::move(request));

    REQUIRE(resolved);
    REQUIRE(resolved->issues.size() == 3);
    CHECK(resolved->issues[0].code == "unavailable_action");
    CHECK(resolved->issues[0].action_id == "tui.input.copy");
    CHECK(resolved->issues[1].code == "unavailable_action");
    CHECK(resolved->issues[1].action_id == "tui.altScreen.previousPrompt");
    CHECK(resolved->issues[2].code == "unknown_action");
    CHECK(resolved->issues[2].action_id == "future.action");
    CHECK(resolved->registry->find("tui.input.copy") == nullptr);
    CHECK(resolved->registry->find("tui.altScreen.previousPrompt") == nullptr);
    CHECK(resolved->registry->find("future.action") == nullptr);
    CHECK(resolved->registry->entries().size() == 30);
}

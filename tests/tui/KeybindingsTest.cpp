#include <cch/tui/Keybindings.hpp>

#include "../../third_party/catch2/catch_test_macros.hpp"

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
    "Invalid overrides retain defaults and key display follows the registry platform",
    "[tui][keybindings][issue57]") {
    tui::KeybindingResolutionRequest request;
    request.definitions = tui::builtin_tui_keybinding_definitions();
    request.overrides = {{.id = "tui.editor.cursorWordLeft", .keys = {"super+left"}}};
    request.platform = tui::KeybindingPlatform::MacOS;

    const auto resolved = tui::resolve_keybindings(std::move(request));

    REQUIRE(resolved);
    REQUIRE(resolved->issues.size() == 1);
    CHECK(resolved->issues.front().code == "invalid_key");
    CHECK((resolved->registry->keys("tui.editor.cursorWordLeft") ==
        std::vector<std::string>{"alt+left", "ctrl+left", "alt+b"}));
    CHECK(resolved->registry->key_text("tui.editor.cursorWordLeft") ==
        "option+left/ctrl+left/option+b");
}

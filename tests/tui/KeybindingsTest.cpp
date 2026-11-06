#include <cch/tui/Keybindings.hpp>

#include "support/PiTuiEvidence.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <string>
#include <vector>

using namespace cch;

TEST_CASE("Keybinding resolution compares frozen pi 1.3.0 manager cases", "[tui][keybindings][issue1221]") {
    const auto bundle = tests::read_pi_v130_audit_evidence("audit-1221-keybindings", "keybindings.json");
    REQUIRE(bundle);
    for (const auto name :
            {"defaults-deduplicate", "overrides-disable-and-conflict", "unknown-claim-and-default-overlap"}) {
        INFO(name);
        const auto& scenario = tests::find_scenario(*bundle, name);
        tui::KeybindingResolutionRequest request;
        request.definitions = {{.id = "alpha", .default_keys = {"up", "ctrl+b", "up"}, .description = "Alpha"},
                {.id = "beta", .default_keys = {"down"}, .description = "Beta"},
                {.id = "gamma", .default_keys = {}, .description = "Gamma"}};
        if (std::string_view{name} == "overrides-disable-and-conflict") {
            request.overrides = {{.id = "alpha", .keys = {"down", "down"}},
                    {.id = "beta", .keys = {"down"}},
                    {.id = "gamma", .keys = {}}};
        } else if (std::string_view{name} == "unknown-claim-and-default-overlap") {
            request.overrides = {{.id = "alpha", .keys = {"down"}}, {.id = "unknown", .keys = {"down"}}};
        }
        const auto resolved = tui::resolve_keybindings(std::move(request));
        REQUIRE(resolved);
        const auto& expected = scenario.at("expected");
        const auto& sequences = scenario.at("inputs").at("sequences").get_array();
        for (const auto id : {"alpha", "beta", "gamma"}) {
            const auto& frozen_keys = expected.at("resolved").at(id);
            std::vector<std::string> keys;
            if (frozen_keys.holds<std::string>()) {
                keys.push_back(frozen_keys.get_string());
            } else {
                for (const auto& key : frozen_keys.get_array())
                    keys.push_back(key.get_string());
            }
            CHECK(resolved->registry->keys(id) == keys);
            const auto& matches = expected.at("matches").at(id).get_array();
            REQUIRE(matches.size() == sequences.size());
            for (std::size_t i = 0; i < sequences.size(); ++i) {
                const auto event = tui::parse_key(sequences[i].get_string(), false);
                REQUIRE(event);
                CHECK(resolved->registry->matches(*event, id) == matches[i].get_boolean());
            }
        }
        const auto& conflicts = expected.at("conflicts").get_array();
        const auto conflict_count = std::count_if(resolved->issues.begin(),
                resolved->issues.end(),
                [](const auto& issue) { return issue.code == "conflicting_user_key"; });
        CHECK(static_cast<std::size_t>(conflict_count) == conflicts.size());
        if (!conflicts.empty()) {
            const auto conflict = std::find_if(resolved->issues.begin(), resolved->issues.end(), [](const auto& issue) {
                return issue.code == "conflicting_user_key";
            });
            REQUIRE(conflict != resolved->issues.end());
            REQUIRE(conflict->key);
            CHECK(*conflict->key == conflicts.front().at("key").get_string());
            std::string claimants;
            for (const auto& claimant : conflicts.front().at("keybindings").get_array()) {
                if (!claimants.empty()) claimants += ", ";
                claimants += claimant.get_string();
            }
            CHECK(conflict->message ==
                    "key '" + *conflict->key + "' is assigned to multiple user actions: " + claimants);
        }
    }
}

TEST_CASE("Keybinding definitions compare the complete frozen pi 1.3.0 table", "[tui][keybindings][issue1221]") {
    const auto bundle = tests::read_pi_v130_audit_evidence("audit-1221-keybindings", "keybindings.json");
    REQUIRE(bundle);
    const auto& frozen = tests::find_scenario(*bundle, "builtin-definitions").at("expected").get_object();
    auto definitions = tui::builtin_tui_keybinding_definitions();
    const auto fullscreen = tui::alt_screen_keybinding_definitions();
    definitions.insert(definitions.end(), fullscreen.begin(), fullscreen.end());
    std::vector<std::string> missing, keys_differ, descriptions_differ, extra;
    for (const auto& [id, definition] : frozen) {
        const auto found =
                std::find_if(definitions.begin(), definitions.end(), [&](const auto& d) { return d.id == id; });
        if (found == definitions.end()) {
            missing.push_back(id);
            continue;
        }
        std::vector<std::string> keys;
        const auto& defaults = definition.at("defaultKeys");
        if (defaults.holds<std::string>())
            keys.push_back(defaults.get_string());
        else
            for (const auto& key : defaults.get_array())
                keys.push_back(key.get_string());
        if (found->default_keys != keys) keys_differ.push_back(id);
        if (found->description != definition.at("description").get_string()) descriptions_differ.push_back(id);
    }
    for (const auto& definition : definitions) {
        if (!frozen.contains(definition.id)) extra.push_back(definition.id);
    }
    std::sort(missing.begin(), missing.end());
    std::sort(keys_differ.begin(), keys_differ.end());
    std::sort(descriptions_differ.begin(), descriptions_differ.end());
    std::sort(extra.begin(), extra.end());
    CHECK(missing == std::vector<std::string>{"tui.input.copy"});
    CHECK((keys_differ == std::vector<std::string>{"tui.editor.pageDown", "tui.editor.pageUp"}));
    CHECK((descriptions_differ == std::vector<std::string>{"tui.editor.historyNext", "tui.editor.historyPrevious"}));
    CHECK(extra.empty());
    CHECK(definitions.size() + missing.size() == frozen.size());
}

TEST_CASE("Keybinding context replacement compares frozen manager rebuild", "[tui][keybindings][issue1221]") {
    const auto bundle = tests::read_pi_v130_audit_evidence("audit-1221-keybindings", "keybindings.json");
    REQUIRE(bundle);
    const auto& expected = tests::find_scenario(*bundle, "replace-user-bindings").at("expected");
    const std::vector<tui::KeybindingDefinition> definitions = {{.id = "alpha", .default_keys = {"up", "ctrl+b", "up"}},
            {.id = "beta", .default_keys = {"down"}},
            {.id = "gamma", .default_keys = {}}};
    const auto initial = tui::resolve_keybindings({.definitions = definitions,
            .overrides = {{.id = "alpha", .keys = {"down"}}, {.id = "beta", .keys = {"down"}}}});
    REQUIRE(initial);
    REQUIRE(expected.at("before").at("conflicts").get_array().size() == 1);
    REQUIRE(initial->issues.size() == 1);
    tui::KeybindingsContext context(initial->registry);
    const auto replacement =
            tui::resolve_keybindings({.definitions = definitions, .overrides = {{.id = "alpha", .keys = {"ctrl+b"}}}});
    REQUIRE(replacement);
    context.replace(replacement->registry);
    const auto& after = expected.at("after");
    CHECK(after.at("conflicts").get_array().empty());
    CHECK(replacement->issues.empty());
    for (const auto id : {"alpha", "beta"}) {
        CHECK(context.keys(id) == std::vector<std::string>{after.at("resolved").at(id).get_string()});
    }
    CHECK(context.keys("gamma").empty());
}

TEST_CASE("Keybinding conflicts require two user claims, not an overlapping default", "[tui][keybindings][issue1221]") {
    tui::KeybindingResolutionRequest request;
    request.definitions = {{.id = "alpha", .default_keys = {"up"}}, {.id = "beta", .default_keys = {"down"}}};
    request.overrides = {{.id = "alpha", .keys = {"down"}}};
    const auto result = tui::resolve_keybindings(std::move(request));
    REQUIRE(result);
    CHECK(result->issues.empty());
    CHECK(result->registry->matches(tui::KeyEvent{.key = "down"}, "alpha"));
    CHECK(result->registry->matches(tui::KeyEvent{.key = "down"}, "beta"));
}

TEST_CASE("Keybindings defines history navigation actions", "[tui][keybindings][issue898][spec]") {
    const auto defs = cch::tui::builtin_tui_keybinding_definitions();
    const auto prev =
            std::find_if(defs.begin(), defs.end(), [](const auto& d) { return d.id == "tui.editor.historyPrevious"; });
    REQUIRE(prev != defs.end());
    CHECK(prev->default_keys.empty());

    const auto next =
            std::find_if(defs.begin(), defs.end(), [](const auto& d) { return d.id == "tui.editor.historyNext"; });
    REQUIRE(next != defs.end());
    CHECK(next->default_keys.empty());
}

TEST_CASE("Keybinding resolution replaces defaults with canonical user alternatives",
        "[tui][keybindings][issue57][spec]") {
    tui::KeybindingResolutionRequest request;
    request.definitions = tui::builtin_tui_keybinding_definitions();
    request.overrides = {
            {.id = "tui.input.submit", .keys = {"ctrl+enter", "return", "enter"}},
            {.id = "tui.input.newLine", .keys = {}},
    };

    const auto resolved = tui::resolve_keybindings(std::move(request));

    REQUIRE(resolved);
    CHECK((resolved->registry->keys("tui.input.submit") == std::vector<std::string>{"ctrl+enter", "enter"}));
    CHECK(resolved->registry->keys("tui.input.newLine").empty());
    CHECK(resolved->registry->matches(tui::KeyEvent{.key = "enter", .ctrl = true}, "tui.input.submit"));
    CHECK_FALSE(resolved->registry->matches(tui::KeyEvent{.key = "enter", .shift = true}, "tui.input.newLine"));
}

TEST_CASE("User key conflicts preserve context defaults and resolve by "
          "candidate order",
        "[tui][keybindings][issue57][spec]") {
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
        "Invalid overrides retain defaults and key display follows the registry", "[tui][keybindings][issue57][spec]") {
    tui::KeybindingResolutionRequest request;
    request.definitions = tui::builtin_tui_keybinding_definitions();
    request.overrides = {{.id = "tui.editor.cursorWordLeft", .keys = {"invalidmodifier+left"}}};

    const auto resolved = tui::resolve_keybindings(std::move(request));

    REQUIRE(resolved);
    REQUIRE(resolved->issues.size() == 1);
    CHECK(resolved->issues.front().code == "invalid_key");
    CHECK((resolved->registry->keys("tui.editor.cursorWordLeft") ==
            std::vector<std::string>{"alt+left", "ctrl+left", "alt+b"}));
    CHECK(resolved->registry->key_text("tui.editor.cursorWordLeft") == "alt+left/ctrl+left/alt+b");
}

TEST_CASE("Builtin table is exactly the frozen pi default table at 83114817", "[tui][keybindings][issue382][spec]") {
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
            {"tui.editor.historyPrevious", {}, "Previous history item"},
            {"tui.editor.historyNext", {}, "Next history item"},
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
    CHECK(editor == 23);
    CHECK(input == 3);
    CHECK(select == 6);

    const auto registry = tui::default_tui_keybindings();
    CHECK(registry->entries().size() == 32);
    for (const auto& entry : kFrozenTable) {
        CHECK(registry->keys(entry.id) == entry.keys);
    }
}

TEST_CASE("Known-but-unassembled IDs are recognized and never become no-op bindings",
        "[tui][keybindings][issue382][spec]") {
    constexpr std::array<std::string_view, 5> kUnassembled{
            "tui.input.copy",
            "tui.altScreen.halfPageUp",
            "tui.altScreen.halfPageDown",
            "tui.altScreen.lineUp",
            "tui.altScreen.lineDown",
    };
    for (const auto id : kUnassembled) {
        CHECK(tui::is_known_unassembled_tui_keybinding(id));
    }
    // Assembled by the fullscreen keybinding manager (#1004, #1011).
    CHECK_FALSE(tui::is_known_unassembled_tui_keybinding("tui.altScreen.previousPrompt"));
    CHECK_FALSE(tui::is_known_unassembled_tui_keybinding("tui.altScreen.nextPrompt"));
    CHECK_FALSE(tui::is_known_unassembled_tui_keybinding("tui.altScreen.pageUp"));
    CHECK_FALSE(tui::is_known_unassembled_tui_keybinding("tui.altScreen.pageDown"));
    CHECK_FALSE(tui::is_known_unassembled_tui_keybinding("tui.altScreen.top"));
    CHECK_FALSE(tui::is_known_unassembled_tui_keybinding("tui.altScreen.bottom"));
    CHECK_FALSE(tui::is_known_unassembled_tui_keybinding("tui.altScreen.search"));
    CHECK_FALSE(tui::is_known_unassembled_tui_keybinding("tui.altScreen.searchNext"));
    CHECK_FALSE(tui::is_known_unassembled_tui_keybinding("tui.altScreen.searchPrevious"));
    CHECK_FALSE(tui::is_known_unassembled_tui_keybinding("tui.altScreen.searchClose"));
    CHECK_FALSE(tui::is_known_unassembled_tui_keybinding("tui.input.submit"));
    CHECK_FALSE(tui::is_known_unassembled_tui_keybinding("tui.editor.cursorUp"));
    CHECK_FALSE(tui::is_known_unassembled_tui_keybinding("tui.altScreen.zoom"));
    CHECK_FALSE(tui::is_known_unassembled_tui_keybinding("future.action"));

    const auto definitions = tui::builtin_tui_keybinding_definitions();
    const auto fullscreen = tui::alt_screen_keybinding_definitions();
    for (const auto id :
            {std::string_view{"tui.altScreen.previousPrompt"}, std::string_view{"tui.altScreen.nextPrompt"}}) {
        CHECK(std::any_of(
                fullscreen.begin(), fullscreen.end(), [id](const auto& definition) { return definition.id == id; }));
    }
    for (const auto id : kUnassembled) {
        const auto found = std::find_if(
                definitions.begin(), definitions.end(), [id](const auto& definition) { return definition.id == id; });
        CHECK(found == definitions.end());
    }
    const auto registry = tui::default_tui_keybindings();
    for (const auto id : kUnassembled) {
        CHECK(registry->find(id) == nullptr);
        CHECK(registry->keys(id).empty());
        CHECK_FALSE(registry->matches(tui::KeyEvent{.key = "c", .ctrl = true}, id));
    }
}

TEST_CASE("Known-but-unassembled overrides diagnose as unavailable, unknown "
          "ids stay unknown",
        "[tui][keybindings][issue382][spec]") {
    tui::KeybindingResolutionRequest request;
    request.definitions = tui::builtin_tui_keybinding_definitions();
    request.overrides = {
            {.id = "tui.input.copy", .keys = {"ctrl+c"}},
            {.id = "tui.altScreen.halfPageUp", .keys = {"ctrl+shift+up"}},
            {.id = "future.action", .keys = {"f1"}},
    };

    const auto resolved = tui::resolve_keybindings(std::move(request));

    REQUIRE(resolved);
    REQUIRE(resolved->issues.size() == 3);
    CHECK(resolved->issues[0].code == "unavailable_action");
    CHECK(resolved->issues[0].action_id == "tui.input.copy");
    CHECK(resolved->issues[1].code == "unavailable_action");
    CHECK(resolved->issues[1].action_id == "tui.altScreen.halfPageUp");
    CHECK(resolved->issues[2].code == "unknown_action");
    CHECK(resolved->issues[2].action_id == "future.action");
    CHECK(resolved->registry->find("tui.input.copy") == nullptr);
    CHECK(resolved->registry->find("tui.altScreen.halfPageUp") == nullptr);
    CHECK(resolved->registry->find("future.action") == nullptr);
    CHECK(resolved->registry->entries().size() == 32);
}

TEST_CASE("resolved keybindings match shifted-symbol reports by produced and physical identity",
        "[tui][keybindings][issue1122][spec]") {
    // Frozen pi-v1.3.0 keys-and-terminal-replies.json:
    // shifted-symbol-keys-match-produced-character. The registry is the public
    // shortcut dispatch caller: a ctrl++ binding fires for both the Kitty
    // ctrl+shift+= report and the modifyOtherKeys ctrl+plus report, while a
    // produced-symbol "+" binding fires only for the unmodified shifted "=".
    tui::KeybindingResolutionRequest request;
    request.definitions = tui::builtin_tui_keybinding_definitions();
    request.overrides = {
            {.id = "tui.editor.historyPrevious", .keys = {"ctrl++"}},
            {.id = "tui.editor.historyNext", .keys = {"+"}},
    };
    const auto resolved = tui::resolve_keybindings(std::move(request));
    REQUIRE(resolved);

    const auto kitty_ctrl_plus = tui::parse_key("\x1b[61:43;6u", true);
    REQUIRE(kitty_ctrl_plus);
    CHECK(resolved->registry->matches(*kitty_ctrl_plus, "tui.editor.historyPrevious"));
    CHECK_FALSE(resolved->registry->matches(*kitty_ctrl_plus, "tui.editor.historyNext"));

    const auto modify_other_keys_ctrl_plus = tui::parse_key("\x1b[27;6;43~", true);
    REQUIRE(modify_other_keys_ctrl_plus);
    CHECK(resolved->registry->matches(*modify_other_keys_ctrl_plus, "tui.editor.historyPrevious"));
    CHECK_FALSE(resolved->registry->matches(*modify_other_keys_ctrl_plus, "tui.editor.historyNext"));

    const auto kitty_shift_plus = tui::parse_key("\x1b[61:43;2u", true);
    REQUIRE(kitty_shift_plus);
    CHECK_FALSE(resolved->registry->matches(*kitty_shift_plus, "tui.editor.historyPrevious"));
    CHECK(resolved->registry->matches(*kitty_shift_plus, "tui.editor.historyNext"));
}

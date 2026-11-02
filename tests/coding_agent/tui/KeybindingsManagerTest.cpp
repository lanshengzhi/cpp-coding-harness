#include "coding_agent/tui/KeybindingsManager.hpp"
#include "coding_agent/tui/SlashCommandEffects.hpp"

#include "support/TempWorkspace.hpp"

#include <cch/tui/Keys.hpp>
#include <cch/tui/Text.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

using namespace cch;

namespace {

[[nodiscard]] std::string fixture_text(std::string_view name) {
    std::ifstream input(
        std::filesystem::path(CCH_SOURCE_DIR) / "tests" / "fixtures" / "keybindings" / name);
    std::ostringstream content;
    content << input.rdbuf();
    return content.str();
}

/// pi's full 43-action `AppKeybindings` table (see the catalog case below).
constexpr std::array<std::string_view, 43> kAppActionCatalog{
        "app.interrupt",
        "app.clear",
        "app.exit",
        "app.suspend",
        "app.thinking.cycle",
        "app.thinking.save",
        "app.model.cycleForward",
        "app.model.cycleBackward",
        "app.model.select",
        "app.tools.expand",
        "app.thinking.toggle",
        "app.session.toggleNamedFilter",
        "app.editor.external",
        "app.message.copy",
        "app.message.followUp",
        "app.message.dequeue",
        "app.clipboard.pasteImage",
        "app.session.new",
        "app.session.tree",
        "app.session.fork",
        "app.session.resume",
        "app.tree.foldOrUp",
        "app.tree.unfoldOrDown",
        "app.tree.editLabel",
        "app.tree.toggleLabelTimestamp",
        "app.session.togglePath",
        "app.session.toggleSort",
        "app.session.rename",
        "app.session.delete",
        "app.session.deleteNoninvasive",
        "app.models.save",
        "app.models.enableAll",
        "app.models.clearAll",
        "app.models.toggleProvider",
        "app.models.reorderUp",
        "app.models.reorderDown",
        "app.tree.filter.default",
        "app.tree.filter.noTools",
        "app.tree.filter.userOnly",
        "app.tree.filter.labeledOnly",
        "app.tree.filter.all",
        "app.tree.filter.cycleForward",
        "app.tree.filter.cycleBackward",
};

[[nodiscard]] bool has_diagnostic(
    const coding_agent::tui::KeybindingsManagerResult& manager,
    std::string_view code) {
    return std::any_of(manager.diagnostics.begin(), manager.diagnostics.end(), [code](const auto& diagnostic) {
        return diagnostic.code == code;
    });
}

} // namespace

TEST_CASE("Keybindings manager reads only the Agent Config Directory and skips unavailable ids",
        "[coding_agent][keybindings][issue57][spec]") {
    tests::TempWorkspace config;
    tests::TempWorkspace decoy;
    config.write("keybindings.json", fixture_text("pi-864b35c.json"));
    decoy.write(".config/pike/agent/keybindings.json", R"({"tui.input.submit":"f12"})");

    coding_agent::tui::KeybindingsManagerRequest request;
    request.agent_config_directory = config.path();
    const auto manager = coding_agent::tui::load_keybindings_manager(std::move(request));

    REQUIRE(manager);
    CHECK(manager->registry->matches(
        tui::KeyEvent{.key = "enter", .ctrl = true},
        "tui.input.submit"));
    CHECK_FALSE(manager->registry->matches(tui::KeyEvent{.key = "enter"}, "tui.input.submit"));
    CHECK(manager->registry->matches(tui::KeyEvent{.key = "enter"}, "tui.input.newLine"));
    CHECK(manager->registry->keys("tui.editor.cursorUp") == std::vector<std::string>{"up"});
    CHECK(manager->registry->find("app.model.select") == nullptr);
    CHECK(has_diagnostic(*manager, "invalid_key"));
    CHECK(has_diagnostic(*manager, "unavailable_action"));
    CHECK(has_diagnostic(*manager, "unknown_action"));
}

TEST_CASE("Hotkey help and hints expose the exact registry used for dispatch",
        "[coding_agent][keybindings][issue57][spec]") {
    tests::TempWorkspace config;
    config.write("keybindings.json", R"({"tui.select.confirm":["ctrl+enter","f2"]})");

    coding_agent::tui::KeybindingsManagerRequest request;
    request.agent_config_directory = config.path();
    const auto manager = coding_agent::tui::load_keybindings_manager(std::move(request));

    REQUIRE(manager);
    const auto entries = coding_agent::tui::hotkey_help_entries(*manager->registry);
    const auto found = std::find_if(entries.begin(), entries.end(), [](const auto& entry) {
        return entry.id == "tui.select.confirm";
    });
    REQUIRE(found != entries.end());
    CHECK(found->keys == "ctrl+enter/f2");
    CHECK(coding_agent::tui::key_hint(*manager->registry, "tui.select.confirm", "choose") ==
        "ctrl+enter/f2 choose");
    CHECK(manager->registry->matches(
        tui::KeyEvent{.key = "enter", .ctrl = true},
        found->id));
    CHECK(coding_agent::tui::format_hotkeys_text(*manager->registry).find("enter  Send message") != std::string::npos);
}

TEST_CASE("Keybindings manager diagnoses malformed values and user conflicts without installing them",
        "[coding_agent][keybindings][issue57][spec]") {
    tests::TempWorkspace config;
    config.write("keybindings.json", R"({
        "tui.editor.cursorUp":"down",
        "tui.select.up":"down",
        "tui.input.submit":42
    })");

    coding_agent::tui::KeybindingsManagerRequest request;
    request.agent_config_directory = config.path();
    const auto manager = coding_agent::tui::load_keybindings_manager(std::move(request));

    REQUIRE(manager);
    CHECK(has_diagnostic(*manager, "conflicting_user_key"));
    CHECK(has_diagnostic(*manager, "invalid_binding_value"));
    CHECK(manager->registry->keys("tui.input.submit") == std::vector<std::string>{"enter"});
    CHECK(manager->registry->keys("tui.editor.cursorDown") == std::vector<std::string>{"down"});
}

TEST_CASE("Known application defaults are installed only for concretely assembled actions",
        "[coding_agent][keybindings][issue57][spec]") {
    constexpr std::array<std::string_view, 1> kSuspend{"app.suspend"};
    const auto definitions = coding_agent::tui::app_keybinding_definitions(kSuspend);
    REQUIRE(definitions);
    REQUIRE(definitions->size() == 1);
    CHECK(definitions->front().default_keys == std::vector<std::string>{"ctrl+z"});

    coding_agent::tui::KeybindingsManagerRequest request;
    request.application_definitions = *definitions;
    const auto manager = coding_agent::tui::load_keybindings_manager(std::move(request));
    REQUIRE(manager);
    const auto* suspend = manager->registry->find("app.suspend");
    REQUIRE(suspend != nullptr);
    CHECK(manager->registry->matches(
        tui::KeyEvent{.key = "z", .ctrl = true},
        "app.suspend"));
    const auto entries = coding_agent::tui::hotkey_help_entries(*manager->registry);
    const auto help = std::find_if(entries.begin(), entries.end(), [](const auto& entry) {
        return entry.id == "app.suspend";
    });
    REQUIRE(help != entries.end());
    CHECK(help->keys == "ctrl+z");
}

TEST_CASE("app.message.followUp and app.message.dequeue carry pi's default keys and help text",
        "[coding_agent][keybindings][issue401][spec]") {
    constexpr std::array<std::string_view, 2> kQueueActions{
        "app.message.followUp",
        "app.message.dequeue",
    };
    const auto definitions = coding_agent::tui::app_keybinding_definitions(
        kQueueActions);
    REQUIRE(definitions);
    REQUIRE(definitions->size() == 2);
    CHECK((*definitions)[0].id == "app.message.followUp");
    CHECK((*definitions)[0].default_keys == std::vector<std::string>{"alt+enter"});
    CHECK((*definitions)[0].description == "Queue follow-up message");
    CHECK((*definitions)[1].id == "app.message.dequeue");
    CHECK((*definitions)[1].default_keys == std::vector<std::string>{"alt+up"});
    CHECK((*definitions)[1].description == "Restore queued messages");

    // The assembled registry resolves the defaults, and dispatch and help
    // both observe them through the exact registry used by the TUI.
    coding_agent::tui::KeybindingsManagerRequest request;
    request.application_definitions = *definitions;
    const auto manager = coding_agent::tui::load_keybindings_manager(std::move(request));
    REQUIRE(manager);
    CHECK(manager->registry->matches(
        tui::KeyEvent{.key = "enter", .alt = true},
        "app.message.followUp"));
    CHECK(manager->registry->matches(
        tui::KeyEvent{.key = "up", .alt = true},
        "app.message.dequeue"));
    CHECK_FALSE(manager->registry->matches(
        tui::KeyEvent{.key = "enter"},
        "app.message.followUp"));
    const auto help_entries = coding_agent::tui::hotkey_help_entries(*manager->registry);
    const auto follow_up = std::find_if(help_entries.begin(), help_entries.end(), [](const auto& entry) {
        return entry.id == "app.message.followUp";
    });
    const auto dequeue = std::find_if(help_entries.begin(), help_entries.end(), [](const auto& entry) {
        return entry.id == "app.message.dequeue";
    });
    REQUIRE(follow_up != help_entries.end());
    REQUIRE(dequeue != help_entries.end());
    CHECK(follow_up->keys == "alt+enter");
    CHECK(dequeue->keys == "alt+up");
}

TEST_CASE("Malformed keybindings documents retain defaults with a bounded diagnostic",
        "[coding_agent][keybindings][issue57][spec]") {
    tests::TempWorkspace config;
    config.write("keybindings.json", "[not an object]");

    coding_agent::tui::KeybindingsManagerRequest request;
    request.agent_config_directory = config.path();
    const auto manager = coding_agent::tui::load_keybindings_manager(std::move(request));

    REQUIRE(manager);
    CHECK(has_diagnostic(*manager, "invalid_keybindings_document"));
    CHECK(manager->registry->keys("tui.input.submit") == std::vector<std::string>{"enter"});
}

TEST_CASE("Known-but-unassembled tui ids are diagnosed while prompt navigation is installed",
        "[coding_agent][keybindings][issue382][issue1011][spec]") {
    tests::TempWorkspace config;
    config.write("keybindings.json", R"({
        "tui.input.copy": "ctrl+c",
        "tui.altScreen.pageUp": ["pageUp"],
        "tui.altScreen.search": "ctrl+f",
        "tui.altScreen.searchNext": "f3",
        "tui.altScreen.searchPrevious": "shift+f3",
        "tui.altScreen.searchClose": "escape",
        "tui.altScreen.previousPrompt": "ctrl+shift+up"
    })");

    coding_agent::tui::KeybindingsManagerRequest request;
    request.agent_config_directory = config.path();
    const auto manager = coding_agent::tui::load_keybindings_manager(std::move(request));

    REQUIRE(manager);
    const auto unavailable = std::count_if(
        manager->diagnostics.begin(),
        manager->diagnostics.end(),
        [](const auto& diagnostic) { return diagnostic.code == "unavailable_action"; });
    CHECK(unavailable == 1);
    CHECK_FALSE(has_diagnostic(*manager, "unknown_action"));
    CHECK(manager->registry->find("tui.input.copy") == nullptr);
    REQUIRE(manager->registry->find("tui.altScreen.pageUp") != nullptr);
    CHECK(manager->registry->keys("tui.altScreen.pageUp") == std::vector<std::string>{"pageUp"});
    CHECK(manager->registry->keys("tui.altScreen.search") == std::vector<std::string>{"ctrl+f"});
    CHECK(manager->registry->keys("tui.altScreen.searchNext") == std::vector<std::string>{"f3"});
    CHECK(manager->registry->keys("tui.altScreen.searchPrevious") == std::vector<std::string>{"shift+f3"});
    CHECK(manager->registry->keys("tui.altScreen.searchClose") == std::vector<std::string>{"escape"});
    CHECK(manager->registry->keys("tui.altScreen.previousPrompt") == std::vector<std::string>{"shift+ctrl+up"});
    CHECK(manager->registry->entries().size() == 42);
    const auto text = coding_agent::tui::format_hotkeys_text(*manager->help);
    CHECK(text.find("tui.input.copy") == std::string::npos);
    CHECK(text.find("app.model.select") == std::string::npos);
}

TEST_CASE("fullscreen keybindings assemble viewport, prompt navigation and search actions",
        "[coding_agent][keybindings][issue1004][issue1010][issue1011]") {
    coding_agent::tui::KeybindingsManagerRequest request;
    const auto manager = coding_agent::tui::load_keybindings_manager(std::move(request));

    REQUIRE(manager);
    CHECK(manager->registry->keys("tui.altScreen.pageUp") == std::vector<std::string>{"pageUp"});
    CHECK(manager->registry->keys("tui.altScreen.pageDown") == std::vector<std::string>{"pageDown"});
    CHECK(manager->registry->keys("tui.altScreen.top") == std::vector<std::string>{"ctrl+home"});
    CHECK(manager->registry->keys("tui.altScreen.bottom") == std::vector<std::string>{"ctrl+end"});
    CHECK(manager->registry->find("tui.altScreen.search") != nullptr);
    CHECK(manager->registry->keys("tui.altScreen.search") == std::vector<std::string>{"shift+ctrl+f"});
    CHECK(manager->registry->find("tui.altScreen.searchNext") != nullptr);
    CHECK(manager->registry->keys("tui.altScreen.searchNext") == std::vector<std::string>{"enter", "ctrl+g"});
    CHECK(manager->registry->find("tui.altScreen.searchPrevious") != nullptr);
    CHECK(manager->registry->keys("tui.altScreen.searchPrevious") ==
            std::vector<std::string>{"shift+enter", "shift+ctrl+g"});
    CHECK(manager->registry->find("tui.altScreen.searchClose") != nullptr);
    CHECK(manager->registry->keys("tui.altScreen.searchClose") == std::vector<std::string>{"escape"});
    CHECK(manager->registry->keys("tui.altScreen.previousPrompt") ==
            std::vector<std::string>{"shift+ctrl+up", "ctrl+up"});
    CHECK(manager->registry->keys("tui.altScreen.nextPrompt") ==
            std::vector<std::string>{"shift+ctrl+down", "ctrl+down"});
}

TEST_CASE("Keybindings manager bounds diagnostics and redacts invalid key text",
        "[coding_agent][keybindings][issue57][spec]") {
    tests::TempWorkspace config;
    std::string json = "{";
    for (int index = 0; index < 70; ++index) {
        if (index != 0) json += ',';
        json += "\"unknown.sk-abcdefghijklmnopqrstuvwxyz123456-" + std::to_string(index) + "\":\"f1\"";
    }
    json += '}';
    config.write("keybindings.json", json);

    coding_agent::tui::KeybindingsManagerRequest request;
    request.agent_config_directory = config.path();
    const auto manager = coding_agent::tui::load_keybindings_manager(std::move(request));

    REQUIRE(manager);
    REQUIRE(manager->diagnostics.size() == 64);
    CHECK(manager->diagnostics.back().code == "diagnostics_truncated");
    for (const auto& diagnostic : manager->diagnostics) {
        CHECK(diagnostic.message.size() <= 1024);
        CHECK(diagnostic.message.find("sk-abcdefghijklmnopqrstuvwxyz123456") == std::string::npos);
    }
}

TEST_CASE("The app layer adopts the full 43-action AppKeybindings table with pi-verbatim descriptions",
        "[coding_agent][keybindings][issue419][issue774][spec]") {
    // The full 43-action catalog (pi core/keybindings.ts; the #36 baseline at
    // 83114817 plus v0.87.1's `app.thinking.save` for the thinking selector's
    // save-as-default action): every action resolves with its pi-verbatim
    // description.
    const auto definitions = coding_agent::tui::app_keybinding_definitions(kAppActionCatalog);
    REQUIRE(definitions);
    REQUIRE(definitions->size() == 43);

    const auto find = [&definitions](std::string_view id) -> const cch::tui::KeybindingDefinition* {
        for (const auto& definition : *definitions) {
            if (definition.id == id) return &definition;
        }
        return nullptr;
    };
    // Spot-check pi-verbatim descriptions and default keys.
    CHECK(find("app.tree.filter.default")->description == "Tree filter: default view");
    CHECK(find("app.tree.filter.cycleBackward")->description == "Tree filter: cycle backward");
    CHECK(find("app.models.toggleProvider")->description == "Toggle all models for provider");
    CHECK(find("app.thinking.save")->description == "Save thinking level");
    CHECK(find("app.thinking.save")->default_keys == std::vector<std::string>{"ctrl+s"});
    CHECK(find("app.message.followUp")->description == "Queue follow-up message");
    CHECK(find("app.message.copy")->description == "Copy selection or last assistant message");
    CHECK(find("app.message.followUp")->default_keys == std::vector<std::string>{"alt+enter"});
    CHECK(find("app.clipboard.pasteImage")->description == "Paste image from clipboard (text fallback)");
    CHECK(find("app.session.new")->default_keys.empty());
    // Unknown ids still fail rather than creating placeholders.
    constexpr std::array<std::string_view, 1> kUnknown{"app.does.not.exist"};
    CHECK_FALSE(coding_agent::tui::app_keybinding_definitions(
        kUnknown));
}

TEST_CASE("/hotkeys renders only the assembled subset from the resolved registry",
        "[coding_agent][keybindings][issue419][spec]") {
    // The main-editor + selector-scoped assembled set: app.interrupt plus the
    // app.session.* recognized-but-unbound ids. Everything else stays out of
    // the registry (never a no-op binding).
    constexpr std::array<std::string_view, 5> kAssembled{
        "app.interrupt",
        "app.session.new",
        "app.session.tree",
        "app.session.fork",
        "app.session.resume",
    };
    auto definitions = coding_agent::tui::app_keybinding_definitions(
        kAssembled);
    REQUIRE(definitions);
    coding_agent::tui::KeybindingsManagerRequest request;
    request.application_definitions = std::move(*definitions);
    const auto manager = coding_agent::tui::load_keybindings_manager(std::move(request));
    REQUIRE(manager);

    // Unassembled application ids are absent from the registry and help.
    CHECK(manager->registry->find("app.model.select") == nullptr);
    const auto entries = coding_agent::tui::hotkey_help_entries(*manager->registry);
    for (const auto& entry : entries) {
        CHECK(entry.id != "app.model.select");
        CHECK(entry.id != "app.tree.filter.all");
    }
    // The recognized-but-unbound app.session.* ids are present as Unbound
    // (never dropped silently) while the unassembled set never appears.
    const auto session_new = std::find_if(entries.begin(), entries.end(), [](const auto& entry) {
        return entry.id == "app.session.new";
    });
    REQUIRE(session_new != entries.end());
    CHECK(session_new->keys == "Unbound");
    const auto help_text = coding_agent::tui::format_hotkeys_text(*manager->help);
    CHECK(help_text.find("Unbound  Start a new session") != std::string::npos);
    CHECK(help_text.find("Unbound  Open session tree") != std::string::npos);
    CHECK(help_text.find("Unbound  Fork current session") != std::string::npos);
    CHECK(help_text.find("Unbound  Resume a session") != std::string::npos);
    CHECK(help_text.find("Open model selector") == std::string::npos);

    // The assembled main-editor action renders like dispatch observes it.
    const auto interrupt = std::find_if(entries.begin(), entries.end(), [](const auto& entry) {
        return entry.id == "app.interrupt";
    });
    REQUIRE(interrupt != entries.end());
    CHECK(interrupt->keys == "escape");
    CHECK(coding_agent::tui::key_hint(
              *manager->registry, "app.interrupt", "interrupt") == "escape interrupt");
}

TEST_CASE("/hotkeys shows both claims for a user key conflict", "[coding_agent][keybindings][issue796][spec]") {
    tests::TempWorkspace config;
    config.write("keybindings.json", R"({"app.clear":"f1","app.exit":"f1"})");
    constexpr std::array<std::string_view, 2> kAssembled{"app.clear", "app.exit"};
    const auto definitions = coding_agent::tui::app_keybinding_definitions(kAssembled);
    REQUIRE(definitions);
    coding_agent::tui::KeybindingsManagerRequest request;
    request.agent_config_directory = config.path();
    request.application_definitions = *definitions;
    const auto manager = coding_agent::tui::load_keybindings_manager(std::move(request));
    REQUIRE(manager);
    CHECK(has_diagnostic(*manager, "conflicting_user_key"));
    const auto text = coding_agent::tui::format_hotkeys_text(*manager->help);
    CHECK(text.find("f1  Clear editor (first) / exit (second)") != std::string::npos);
    CHECK(text.find("f1  Exit (when editor is empty)") != std::string::npos);
}

TEST_CASE("/hotkeys chat block follows pi sections over the effective registry",
        "[coding_agent][keybindings][issue796][spec]") {
    // Default registry with no application actions assembled: pi sections
    // render with effective keys; unavailable application rows are skipped.
    coding_agent::tui::KeybindingsManagerRequest request;
    const auto manager = coding_agent::tui::load_keybindings_manager(std::move(request));
    REQUIRE(manager);
    const auto text = coding_agent::tui::format_hotkeys_text(*manager->help);
    CHECK(text.find("Keyboard Shortcuts") != std::string::npos);
    const auto navigation = text.find("Navigation");
    const auto editing = text.find("Editing");
    const auto other = text.find("Other");
    REQUIRE(navigation != std::string::npos);
    REQUIRE(editing != std::string::npos);
    REQUIRE(other != std::string::npos);
    CHECK(navigation < editing);
    CHECK(editing < other);
    CHECK(text.find("Move cursor / browse history") != std::string::npos);
    CHECK(text.find("enter  Send message") != std::string::npos);
    CHECK(text.find("/  Slash commands") != std::string::npos);
    CHECK(text.find("Exit (when editor is empty)") == std::string::npos);

    // The component seam and the inline chat block use one formatter. The
    // narrow render still contains all three section headings rather than
    // dropping the earlier sections at the right edge.
    auto view = std::make_unique<cch::tui::Text>(coding_agent::tui::format_hotkeys_text(*manager->help));
    REQUIRE(view);
    const auto wide_rendered = view->render(200);
    REQUIRE(wide_rendered);
    std::string wide_text;
    for (const auto& line : wide_rendered->lines) {
        auto trimmed = line;
        while (!trimmed.empty() && trimmed.back() == ' ')
            trimmed.pop_back();
        wide_text += trimmed;
        wide_text.push_back('\n');
    }
    CHECK(wide_text.find("Move cursor / browse history") != std::string::npos);
    CHECK(wide_text.find("enter  Send message") != std::string::npos);
    CHECK(wide_text.find("Exit (when editor is empty)") == std::string::npos);

    const auto rendered = view->render(24);
    REQUIRE(rendered);
    std::string narrow_text;
    for (const auto& line : rendered->lines) {
        narrow_text += line;
        narrow_text.push_back('\n');
    }
    CHECK(narrow_text.find("Navigation") != std::string::npos);
    CHECK(narrow_text.find("Editing") != std::string::npos);
    CHECK(narrow_text.find("Other") != std::string::npos);
    CHECK(narrow_text.find("Keyboard Shortcuts") != std::string::npos);

    // A user override shows up verbatim on its pi row, and assembled queue
    // actions render with pi's default keys.
    tests::TempWorkspace config;
    config.write("keybindings.json", R"({"app.exit":"f6"})");
    constexpr std::array<std::string_view, 4> kAssembled{
            "app.exit", "app.message.copy", "app.message.followUp", "app.message.dequeue"};
    auto definitions = coding_agent::tui::app_keybinding_definitions(kAssembled);
    REQUIRE(definitions);
    coding_agent::tui::KeybindingsManagerRequest remapped;
    remapped.agent_config_directory = config.path();
    remapped.application_definitions = std::move(*definitions);
    const auto remapped_manager = coding_agent::tui::load_keybindings_manager(std::move(remapped));
    REQUIRE(remapped_manager);
    const auto remapped_text = coding_agent::tui::format_hotkeys_text(*remapped_manager->help);
    CHECK(remapped_text.find("f6  Exit (when editor is empty)") != std::string::npos);
    auto remapped_view =
            std::make_unique<cch::tui::Text>(coding_agent::tui::format_hotkeys_text(*remapped_manager->help));
    REQUIRE(remapped_view);
    const auto remapped_rendered = remapped_view->render(200);
    REQUIRE(remapped_rendered);
    const auto remapped_component_text = std::accumulate(remapped_rendered->lines.begin(),
            remapped_rendered->lines.end(),
            std::string{},
            [](std::string text, const auto& line) {
                auto trimmed = line;
                while (!trimmed.empty() && trimmed.back() == ' ')
                    trimmed.pop_back();
                text += trimmed;
                text.push_back('\n');
                return text;
            });
    CHECK(remapped_component_text.find("f6  Exit (when editor is empty)") != std::string::npos);
    CHECK(remapped_text.find("ctrl+x  Copy selection or last assistant message") != std::string::npos);
    CHECK(remapped_text.find("alt+enter  Queue follow-up message") != std::string::npos);
    CHECK(remapped_text.find("alt+up  Restore queued messages") != std::string::npos);
}

TEST_CASE("Keybindings manager migrates legacy names, keeps unknown actions and reloads",
        "[coding_agent][keybindings][issue1115][spec]") {
    // fixtures/pi-v1.3.0/audit-1049/configuration-schema.json, scenario
    // "keybindings manager migrates legacy names, keeps unknown actions, drops
    // invalid values and reloads".
    tests::TempWorkspace config;
    config.write("keybindings.json", R"({
        "app.interrupt": "ctrl+q",
        "app.clear": "ctrl+q",
        "app.unknown": "ctrl+u",
        "cursorUp": "ctrl+u",
        "app.model.select": "not-a-key"
    })");

    constexpr std::array<std::string_view, 2> kAssembled{"app.interrupt", "app.clear"};
    auto definitions = coding_agent::tui::app_keybinding_definitions(kAssembled);
    REQUIRE(definitions);
    coding_agent::tui::KeybindingsManagerRequest request;
    request.agent_config_directory = config.path();
    request.application_definitions = std::move(*definitions);
    const auto manager = coding_agent::tui::load_keybindings_manager(std::move(request));
    REQUIRE(manager);

    // The legacy `cursorUp` entry migrates to `tui.editor.cursorUp` and is
    // installed, so dispatch sees the user's key.
    CHECK(manager->registry->keys("tui.editor.cursorUp") == std::vector<std::string>{"ctrl+u"});
    CHECK(manager->registry->keys("app.interrupt") == std::vector<std::string>{"ctrl+q"});
    CHECK(manager->registry->keys("app.clear") == std::vector<std::string>{"ctrl+q"});
    // An unknown id stays in the user map and is never installed.
    CHECK(manager->registry->find("app.unknown") == nullptr);
    // An invalid key value is dropped from both. `app.model.select` is not
    // assembled by this request, so its default is not in the registry either.
    CHECK(manager->registry->find("app.model.select") == nullptr);
    CHECK(has_diagnostic(*manager, "invalid_key"));

    // A duplicate key is pi's `conflicts` entry: `ctrl+q` is claimed by
    // `app.interrupt` and `app.clear`, in that order.
    const auto conflict = std::find_if(manager->diagnostics.begin(),
            manager->diagnostics.end(),
            [](const auto& diagnostic) { return diagnostic.code == "conflicting_user_key"; });
    REQUIRE(conflict != manager->diagnostics.end());
    INFO(conflict->message);
    CHECK(conflict->message.find("ctrl+q") != std::string::npos);
    CHECK(conflict->message.find("app.interrupt, app.clear") != std::string::npos);

    // pi `getUserBindings`: migrated names, retained unknown ids, dropped
    // invalid values, catalog order first then the remaining ids sorted.
    const std::vector<std::string> expected_ids{"tui.editor.cursorUp", "app.interrupt", "app.clear", "app.unknown"};
    REQUIRE(manager->user_bindings.size() == expected_ids.size());
    for (std::size_t index = 0; index < expected_ids.size(); ++index) {
        INFO(index);
        CHECK(manager->user_bindings[index].id == expected_ids[index]);
    }
    CHECK(manager->user_bindings.front().keys == std::vector<std::string>{"ctrl+u"});

    // pi `reload`: the user map is replaced wholesale, so a removed entry stops
    // overriding its action.
    config.write("keybindings.json", R"({"app.interrupt": "escape"})");
    coding_agent::tui::KeybindingsManagerRequest reload_request;
    reload_request.agent_config_directory = config.path();
    reload_request.application_definitions = coding_agent::tui::app_keybinding_definitions(kAssembled).value();
    const auto reloaded = coding_agent::tui::load_keybindings_manager(std::move(reload_request));
    REQUIRE(reloaded);
    CHECK(reloaded->registry->keys("app.interrupt") == std::vector<std::string>{"escape"});
    CHECK(reloaded->registry->keys("tui.editor.cursorUp") == std::vector<std::string>{"up"});
    REQUIRE(reloaded->user_bindings.size() == 1);
    CHECK(reloaded->user_bindings.front().id == "app.interrupt");
}

TEST_CASE("Keybindings manager lets the canonical id win over its legacy spelling",
        "[coding_agent][keybindings][issue1115][spec]") {
    // pi `migrateKeybindingsConfig`: when the stored document carries both the
    // legacy name and its canonical id, the canonical entry is the one kept.
    tests::TempWorkspace config;
    config.write("keybindings.json", R"({"cursorUp": "ctrl+u", "tui.editor.cursorUp": "ctrl+k"})");

    coding_agent::tui::KeybindingsManagerRequest request;
    request.agent_config_directory = config.path();
    const auto manager = coding_agent::tui::load_keybindings_manager(std::move(request));
    REQUIRE(manager);
    CHECK(manager->registry->keys("tui.editor.cursorUp") == std::vector<std::string>{"ctrl+k"});
    REQUIRE(manager->user_bindings.size() == 1);
    CHECK(manager->user_bindings.front().keys == std::vector<std::string>{"ctrl+k"});
}

TEST_CASE("Keybindings manager skips the $schema member and keeps a BOM-prefixed document",
        "[coding_agent][keybindings][issue1115][spec]") {
    // The frozen scenario writes a UTF-8 BOM and pi's `stripBom` tolerates it;
    // `$schema` is a schema-reference member, not an action.
    tests::TempWorkspace config;
    config.write(
            "keybindings.json", std::string{"\xEF\xBB\xBF"} + R"({"$schema": "https://example.invalid/keybindings.json",
        "tui.editor.cursorUp": "ctrl+u"})");

    coding_agent::tui::KeybindingsManagerRequest request;
    request.agent_config_directory = config.path();
    const auto manager = coding_agent::tui::load_keybindings_manager(std::move(request));
    REQUIRE(manager);
    CHECK_FALSE(has_diagnostic(*manager, "unknown_action"));
    CHECK(manager->registry->keys("tui.editor.cursorUp") == std::vector<std::string>{"ctrl+u"});
    REQUIRE(manager->user_bindings.size() == 1);
    CHECK(manager->user_bindings.front().id == "tui.editor.cursorUp");
}

TEST_CASE("the assembled catalog plus the known-unassembled actions is pi's 90-action catalog",
        "[coding_agent][keybindings][issue1115][spec]") {
    // fixtures/pi-v1.3.0/audit-1049/configuration-schema.json, scenario
    // "keybindings manager migrates legacy names, keeps unknown actions, drops
    // invalid values and reloads": pi's effective catalog is 90 actions. This
    // build assembles 85 and knows the remaining five as unavailable, so an
    // assembled action silently disappearing or a new action being invented
    // separates.
    constexpr std::array<std::string_view, 5> kKnownUnassembled{
            "tui.input.copy",
            "tui.altScreen.halfPageUp",
            "tui.altScreen.halfPageDown",
            "tui.altScreen.lineUp",
            "tui.altScreen.lineDown",
    };
    coding_agent::tui::KeybindingsManagerRequest request;
    request.application_definitions = coding_agent::tui::app_keybinding_definitions(kAppActionCatalog).value();
    const auto manager = coding_agent::tui::load_keybindings_manager(std::move(request));
    REQUIRE(manager);
    CHECK(manager->registry->entries().size() == 85);
    for (const auto id : kKnownUnassembled) {
        CHECK(manager->registry->find(id) == nullptr);
    }
    CHECK(manager->registry->entries().size() + kKnownUnassembled.size() == 90);
}

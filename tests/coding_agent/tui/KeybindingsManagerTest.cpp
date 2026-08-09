#include "coding_agent/tui/KeybindingsManager.hpp"

#include "support/TempWorkspace.hpp"

#include <cch/tui/Keys.hpp>

#include "../../../third_party/catch2/catch_test_macros.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
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

[[nodiscard]] bool has_diagnostic(
    const coding_agent::tui::KeybindingsManagerResult& manager,
    std::string_view code) {
    return std::any_of(manager.diagnostics.begin(), manager.diagnostics.end(), [code](const auto& diagnostic) {
        return diagnostic.code == code;
    });
}

} // namespace

TEST_CASE(
    "Keybindings manager reads only the Agent Config Directory and skips unavailable ids",
    "[coding_agent][keybindings][issue57]") {
    tests::TempWorkspace config;
    tests::TempWorkspace decoy;
    config.write("keybindings.json", fixture_text("pi-864b35c.json"));
    decoy.write(".pi/agent/keybindings.json", R"({"tui.input.submit":"f12"})");

    coding_agent::tui::KeybindingsManagerRequest request;
    request.agent_config_directory = config.path();
    request.platform = tui::KeybindingPlatform::Linux;
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

TEST_CASE("Hotkey help and hints expose the exact registry used for dispatch", "[coding_agent][keybindings][issue57]") {
    tests::TempWorkspace config;
    config.write("keybindings.json", R"({"tui.select.confirm":["ctrl+enter","f2"]})");

    coding_agent::tui::KeybindingsManagerRequest request;
    request.agent_config_directory = config.path();
    request.platform = tui::KeybindingPlatform::MacOS;
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
    auto view = coding_agent::tui::make_hotkey_help_view(manager->registry);
    const auto rendered = view->render(100);
    REQUIRE(rendered);
    CHECK(std::any_of(rendered->lines.begin(), rendered->lines.end(), [](const auto& line) {
        return line.find("ctrl+enter/f2") != std::string::npos;
    }));
}

TEST_CASE(
    "Keybindings manager diagnoses malformed values and user conflicts without installing them",
    "[coding_agent][keybindings][issue57]") {
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

TEST_CASE(
    "Known application defaults are installed only for concretely assembled actions",
    "[coding_agent][keybindings][issue57]") {
    constexpr std::array<std::string_view, 1> kSuspend{"app.suspend"};
    const auto linux_definitions = coding_agent::tui::app_keybinding_definitions(
        kSuspend,
        tui::KeybindingPlatform::Linux);
    REQUIRE(linux_definitions);
    REQUIRE(linux_definitions->size() == 1);
    CHECK(linux_definitions->front().default_keys == std::vector<std::string>{"ctrl+z"});
    CHECK(linux_definitions->front().available);

    const auto windows_definitions = coding_agent::tui::app_keybinding_definitions(
        kSuspend,
        tui::KeybindingPlatform::Windows);
    REQUIRE(windows_definitions);
    REQUIRE(windows_definitions->size() == 1);
    CHECK(windows_definitions->front().default_keys.empty());
    CHECK_FALSE(windows_definitions->front().available);

    coding_agent::tui::KeybindingsManagerRequest request;
    request.application_definitions = *windows_definitions;
    request.platform = tui::KeybindingPlatform::Windows;
    const auto manager = coding_agent::tui::load_keybindings_manager(std::move(request));
    REQUIRE(manager);
    const auto* suspend = manager->registry->find("app.suspend");
    REQUIRE(suspend != nullptr);
    CHECK_FALSE(suspend->available);
    CHECK_FALSE(manager->registry->matches(
        tui::KeyEvent{.key = "z", .ctrl = true},
        "app.suspend"));
    const auto entries = coding_agent::tui::hotkey_help_entries(*manager->registry);
    const auto help = std::find_if(entries.begin(), entries.end(), [](const auto& entry) {
        return entry.id == "app.suspend";
    });
    REQUIRE(help != entries.end());
    CHECK(help->keys == "Unavailable on native Windows");
}

TEST_CASE(
    "app.message.followUp and app.message.dequeue carry pi's default keys and help text",
    "[coding_agent][keybindings][issue401]") {
    constexpr std::array<std::string_view, 2> kQueueActions{
        "app.message.followUp",
        "app.message.dequeue",
    };
    const auto definitions = coding_agent::tui::app_keybinding_definitions(
        kQueueActions,
        tui::KeybindingPlatform::Linux);
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
    request.platform = tui::KeybindingPlatform::Linux;
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

TEST_CASE(
    "Malformed keybindings documents retain defaults with a bounded diagnostic",
    "[coding_agent][keybindings][issue57]") {
    tests::TempWorkspace config;
    config.write("keybindings.json", "[not an object]");

    coding_agent::tui::KeybindingsManagerRequest request;
    request.agent_config_directory = config.path();
    const auto manager = coding_agent::tui::load_keybindings_manager(std::move(request));

    REQUIRE(manager);
    CHECK(has_diagnostic(*manager, "invalid_keybindings_document"));
    CHECK(manager->registry->keys("tui.input.submit") == std::vector<std::string>{"enter"});
}

TEST_CASE(
    "Known-but-unassembled tui ids are diagnosed as unavailable and never installed",
    "[coding_agent][keybindings][issue382]") {
    tests::TempWorkspace config;
    config.write("keybindings.json", R"({
        "tui.input.copy": "ctrl+c",
        "tui.altScreen.pageUp": ["pageUp"],
        "tui.altScreen.previousPrompt": "ctrl+shift+up"
    })");

    coding_agent::tui::KeybindingsManagerRequest request;
    request.agent_config_directory = config.path();
    request.platform = tui::KeybindingPlatform::Linux;
    const auto manager = coding_agent::tui::load_keybindings_manager(std::move(request));

    REQUIRE(manager);
    const auto unavailable = std::count_if(
        manager->diagnostics.begin(),
        manager->diagnostics.end(),
        [](const auto& diagnostic) { return diagnostic.code == "unavailable_action"; });
    CHECK(unavailable == 3);
    CHECK_FALSE(has_diagnostic(*manager, "unknown_action"));
    CHECK(manager->registry->find("tui.input.copy") == nullptr);
    CHECK(manager->registry->find("tui.altScreen.pageUp") == nullptr);
    CHECK(manager->registry->find("tui.altScreen.previousPrompt") == nullptr);
    CHECK(manager->registry->entries().size() == 30);
}

TEST_CASE(
    "Keybindings manager bounds diagnostics and redacts invalid key text",
    "[coding_agent][keybindings][issue57]") {
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
    CHECK(manager->diagnostics.size() == 64);
    CHECK(manager->diagnostics.back().code == "diagnostics_truncated");
    for (const auto& diagnostic : manager->diagnostics) {
        CHECK(diagnostic.message.size() <= 1024);
        CHECK(diagnostic.message.find("sk-abcdefghijklmnopqrstuvwxyz123456") == std::string::npos);
    }
}

TEST_CASE(
    "The app layer adopts the full 42-action AppKeybindings table with pi-verbatim descriptions",
    "[coding_agent][keybindings][issue419]") {
    // The full 42-action catalog (pi core/keybindings.ts at 83114817, ADR
    // 0036 G2): every action resolves with its pi-verbatim description.
    constexpr std::array<std::string_view, 42> kAllActions{
        "app.interrupt",
        "app.clear",
        "app.exit",
        "app.suspend",
        "app.thinking.cycle",
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
    const auto definitions = coding_agent::tui::app_keybinding_definitions(
        kAllActions, tui::KeybindingPlatform::Linux);
    REQUIRE(definitions);
    REQUIRE(definitions->size() == 42);

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
    CHECK(find("app.message.followUp")->description == "Queue follow-up message");
    CHECK(find("app.message.followUp")->default_keys == std::vector<std::string>{"alt+enter"});
    CHECK(find("app.clipboard.pasteImage")->description == "Paste image from clipboard (text fallback)");
    CHECK(find("app.session.new")->default_keys.empty());
    // Unknown ids still fail rather than creating placeholders.
    constexpr std::array<std::string_view, 1> kUnknown{"app.does.not.exist"};
    CHECK_FALSE(coding_agent::tui::app_keybinding_definitions(
        kUnknown, tui::KeybindingPlatform::Linux));
}

TEST_CASE(
    "/hotkeys renders only the assembled subset from the resolved registry",
    "[coding_agent][keybindings][issue419]") {
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
        kAssembled, tui::KeybindingPlatform::Linux);
    REQUIRE(definitions);
    coding_agent::tui::KeybindingsManagerRequest request;
    request.application_definitions = std::move(*definitions);
    request.platform = tui::KeybindingPlatform::Linux;
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

    // The assembled main-editor action renders like dispatch observes it.
    const auto interrupt = std::find_if(entries.begin(), entries.end(), [](const auto& entry) {
        return entry.id == "app.interrupt";
    });
    REQUIRE(interrupt != entries.end());
    CHECK(interrupt->keys == "escape");
    CHECK(coding_agent::tui::key_hint(
              *manager->registry, "app.interrupt", "interrupt") == "escape interrupt");
}

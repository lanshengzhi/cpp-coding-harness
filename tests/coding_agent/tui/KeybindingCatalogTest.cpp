#include "coding_agent/tui/KeybindingCatalog.hpp"
#include "coding_agent/tui/KeybindingHelp.hpp"
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
    const coding_agent::tui::KeybindingCatalogResult& catalog,
    std::string_view code) {
    return std::any_of(catalog.diagnostics.begin(), catalog.diagnostics.end(), [code](const auto& diagnostic) {
        return diagnostic.code == code;
    });
}

} // namespace

TEST_CASE(
    "Keybinding catalog reads only the Agent Config Directory and skips unavailable ids",
    "[coding_agent][keybindings][issue57]") {
    tests::TempWorkspace config;
    tests::TempWorkspace decoy;
    config.write("keybindings.json", fixture_text("pi-864b35c.json"));
    decoy.write(".pi/agent/keybindings.json", R"({"tui.input.submit":"f12"})");

    coding_agent::tui::KeybindingCatalogRequest request;
    request.agent_config_directory = config.path();
    request.platform = tui::KeybindingPlatform::Linux;
    const auto catalog = coding_agent::tui::load_keybinding_catalog(std::move(request));

    REQUIRE(catalog);
    CHECK(catalog->registry->matches(
        tui::KeyEvent{.key = "enter", .ctrl = true},
        "tui.input.submit"));
    CHECK_FALSE(catalog->registry->matches(tui::KeyEvent{.key = "enter"}, "tui.input.submit"));
    CHECK(catalog->registry->matches(tui::KeyEvent{.key = "enter"}, "tui.input.newLine"));
    CHECK(catalog->registry->keys("tui.editor.cursorUp") == std::vector<std::string>{"up"});
    CHECK(catalog->registry->find("app.model.select") == nullptr);
    CHECK(has_diagnostic(*catalog, "invalid_key"));
    CHECK(has_diagnostic(*catalog, "unavailable_action"));
    CHECK(has_diagnostic(*catalog, "unknown_action"));
}

TEST_CASE("Hotkey help and hints expose the exact registry used for dispatch", "[coding_agent][keybindings][issue57]") {
    tests::TempWorkspace config;
    config.write("keybindings.json", R"({"tui.select.confirm":["ctrl+enter","f2"]})");

    coding_agent::tui::KeybindingCatalogRequest request;
    request.agent_config_directory = config.path();
    request.platform = tui::KeybindingPlatform::MacOS;
    const auto catalog = coding_agent::tui::load_keybinding_catalog(std::move(request));

    REQUIRE(catalog);
    const auto entries = coding_agent::tui::hotkey_help_entries(*catalog->registry);
    const auto found = std::find_if(entries.begin(), entries.end(), [](const auto& entry) {
        return entry.id == "tui.select.confirm";
    });
    REQUIRE(found != entries.end());
    CHECK(found->keys == "ctrl+enter/f2");
    CHECK(coding_agent::tui::key_hint(*catalog->registry, "tui.select.confirm", "choose") ==
        "ctrl+enter/f2 choose");
    CHECK(catalog->registry->matches(
        tui::KeyEvent{.key = "enter", .ctrl = true},
        found->id));
    auto view = coding_agent::tui::make_hotkey_help_view(catalog->registry);
    const auto rendered = view->render(100);
    REQUIRE(rendered);
    CHECK(std::any_of(rendered->lines.begin(), rendered->lines.end(), [](const auto& line) {
        return line.find("ctrl+enter/f2") != std::string::npos;
    }));
}

TEST_CASE(
    "Keybinding catalog diagnoses malformed values and user conflicts without installing them",
    "[coding_agent][keybindings][issue57]") {
    tests::TempWorkspace config;
    config.write("keybindings.json", R"({
        "tui.editor.cursorUp":"down",
        "tui.select.up":"down",
        "tui.input.submit":42
    })");

    coding_agent::tui::KeybindingCatalogRequest request;
    request.agent_config_directory = config.path();
    const auto catalog = coding_agent::tui::load_keybinding_catalog(std::move(request));

    REQUIRE(catalog);
    CHECK(has_diagnostic(*catalog, "conflicting_user_key"));
    CHECK(has_diagnostic(*catalog, "invalid_binding_value"));
    CHECK(catalog->registry->keys("tui.input.submit") == std::vector<std::string>{"enter"});
    CHECK(catalog->registry->keys("tui.editor.cursorDown") == std::vector<std::string>{"down"});
}

TEST_CASE(
    "Known application defaults are installed only for concretely assembled actions",
    "[coding_agent][keybindings][issue57]") {
    constexpr std::array<std::string_view, 1> kSuspend{"app.suspend"};
    const auto linux_definitions = coding_agent::tui::baseline_application_keybindings(
        kSuspend,
        tui::KeybindingPlatform::Linux);
    REQUIRE(linux_definitions);
    REQUIRE(linux_definitions->size() == 1);
    CHECK(linux_definitions->front().default_keys == std::vector<std::string>{"ctrl+z"});
    CHECK(linux_definitions->front().available);

    const auto windows_definitions = coding_agent::tui::baseline_application_keybindings(
        kSuspend,
        tui::KeybindingPlatform::Windows);
    REQUIRE(windows_definitions);
    REQUIRE(windows_definitions->size() == 1);
    CHECK(windows_definitions->front().default_keys.empty());
    CHECK_FALSE(windows_definitions->front().available);

    coding_agent::tui::KeybindingCatalogRequest request;
    request.application_definitions = *windows_definitions;
    request.platform = tui::KeybindingPlatform::Windows;
    const auto catalog = coding_agent::tui::load_keybinding_catalog(std::move(request));
    REQUIRE(catalog);
    const auto* suspend = catalog->registry->find("app.suspend");
    REQUIRE(suspend != nullptr);
    CHECK_FALSE(suspend->available);
    CHECK_FALSE(catalog->registry->matches(
        tui::KeyEvent{.key = "z", .ctrl = true},
        "app.suspend"));
    const auto entries = coding_agent::tui::hotkey_help_entries(*catalog->registry);
    const auto help = std::find_if(entries.begin(), entries.end(), [](const auto& entry) {
        return entry.id == "app.suspend";
    });
    REQUIRE(help != entries.end());
    CHECK(help->keys == "Unavailable on native Windows");
}

TEST_CASE(
    "Malformed keybinding documents retain defaults with a bounded diagnostic",
    "[coding_agent][keybindings][issue57]") {
    tests::TempWorkspace config;
    config.write("keybindings.json", "[not an object]");

    coding_agent::tui::KeybindingCatalogRequest request;
    request.agent_config_directory = config.path();
    const auto catalog = coding_agent::tui::load_keybinding_catalog(std::move(request));

    REQUIRE(catalog);
    CHECK(has_diagnostic(*catalog, "invalid_keybindings_document"));
    CHECK(catalog->registry->keys("tui.input.submit") == std::vector<std::string>{"enter"});
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

    coding_agent::tui::KeybindingCatalogRequest request;
    request.agent_config_directory = config.path();
    request.platform = tui::KeybindingPlatform::Linux;
    const auto catalog = coding_agent::tui::load_keybinding_catalog(std::move(request));

    REQUIRE(catalog);
    const auto unavailable = std::count_if(
        catalog->diagnostics.begin(),
        catalog->diagnostics.end(),
        [](const auto& diagnostic) { return diagnostic.code == "unavailable_action"; });
    CHECK(unavailable == 3);
    CHECK_FALSE(has_diagnostic(*catalog, "unknown_action"));
    CHECK(catalog->registry->find("tui.input.copy") == nullptr);
    CHECK(catalog->registry->find("tui.altScreen.pageUp") == nullptr);
    CHECK(catalog->registry->find("tui.altScreen.previousPrompt") == nullptr);
    CHECK(catalog->registry->entries().size() == 30);
}

TEST_CASE(
    "Keybinding catalog bounds diagnostics and redacts invalid key text",
    "[coding_agent][keybindings][issue57]") {
    tests::TempWorkspace config;
    std::string json = "{";
    for (int index = 0; index < 70; ++index) {
        if (index != 0) json += ',';
        json += "\"unknown.sk-abcdefghijklmnopqrstuvwxyz123456-" + std::to_string(index) + "\":\"f1\"";
    }
    json += '}';
    config.write("keybindings.json", json);

    coding_agent::tui::KeybindingCatalogRequest request;
    request.agent_config_directory = config.path();
    const auto catalog = coding_agent::tui::load_keybinding_catalog(std::move(request));

    REQUIRE(catalog);
    CHECK(catalog->diagnostics.size() == 64);
    CHECK(catalog->diagnostics.back().code == "diagnostics_truncated");
    for (const auto& diagnostic : catalog->diagnostics) {
        CHECK(diagnostic.message.size() <= 1024);
        CHECK(diagnostic.message.find("sk-abcdefghijklmnopqrstuvwxyz123456") == std::string::npos);
    }
}

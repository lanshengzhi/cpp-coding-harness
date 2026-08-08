#include "coding_agent/BoundedText.hpp"
#include "coding_agent/ProjectResourceLoader.hpp"
#include "coding_agent/tui/ThemeCatalog.hpp"
#include "harness/WorkspaceFileSystem.hpp"
#include "support/TempWorkspace.hpp"
#include <cch/coding_agent/Settings.hpp>
#include <cch/tui/Text.hpp>
#include <cch/tui/Tui.hpp>
#include <cch/tui/VirtualTerminal.hpp>

#include "../../../third_party/catch2/catch_test_macros.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace cch;

namespace {

[[nodiscard]] std::filesystem::path fixture_path(std::string_view name) {
    return std::filesystem::path(CCH_SOURCE_DIR) / "tests" / "fixtures" / "themes" / name;
}

[[nodiscard]] std::string fixture_theme(std::string_view name, std::string_view accent) {
    std::ifstream input(fixture_path("dark.json"));
    std::ostringstream content;
    content << input.rdbuf();
    auto json = content.str();
    const auto replace_once = [&json](std::string_view old_text, std::string_view new_text) {
        const auto position = json.find(old_text);
        REQUIRE(position != std::string::npos);
        json.replace(position, old_text.size(), new_text);
    };
    replace_once(
        "\"name\": \"dark\"",
        std::string{"\"name\": \""} + std::string(name) + "\"");
    replace_once(
        "\"accent\": \"accent\"",
        std::string{"\"accent\": \""} + std::string(accent) + "\"");
    return json;
}

[[nodiscard]] const coding_agent::tui::ThemeResource* find_theme(
    const coding_agent::tui::ThemeCatalogResult& catalog,
    std::string_view name) {
    const auto found = std::find_if(
        catalog.effective_themes.begin(),
        catalog.effective_themes.end(),
        [name](const auto& resource) { return resource.theme.name == name; });
    return found == catalog.effective_themes.end() ? nullptr : &*found;
}

[[nodiscard]] coding_agent::tui::RgbThemeColor accent_of(
    const coding_agent::tui::ResolvedTheme& theme) {
    const auto* color = std::get_if<coding_agent::tui::RgbThemeColor>(
        &coding_agent::tui::color_for(theme, coding_agent::tui::ThemeToken::Accent));
    REQUIRE(color != nullptr);
    return *color;
}

class InvalidatingText final : public tui::Component {
public:
    [[nodiscard]] util::Expected<tui::RenderResult> render(std::size_t) override {
        return tui::RenderResult{.lines = {"base"}};
    }

    void invalidate() override {
        ++invalidations;
    }

    std::size_t invalidations{0};
};

} // namespace

TEST_CASE(
    "Theme catalog resolves explicit project global and built-in precedence",
    "[coding_agent][theme][issue56]") {
    tests::TempWorkspace config;
    tests::TempWorkspace explicit_base;
    config.write("themes/global.json", fixture_theme("shared", "#111111"));
    explicit_base.write("explicit.json", fixture_theme("shared", "#333333"));
    explicit_base.write("later.json", fixture_theme("shared", "#444444"));

    coding_agent::tui::ThemeCatalogRequest request;
    request.agent_config_directory = config.path();
    request.explicit_path_base = explicit_base.path();
    request.explicit_paths = {"explicit.json", "later.json"};
    request.trusted_project_themes = {{
        .label = ".pi/themes/project.json",
        .json = fixture_theme("shared", "#222222"),
    }};
    request.explicit_active_theme = "shared";
    request.user_active_theme = "dark";
    request.terminal_capabilities.appearance = tui::TerminalAppearance::Light;

    const auto catalog = coding_agent::tui::load_theme_catalog(std::move(request));

    REQUIRE(catalog);
    const auto* shared = find_theme(*catalog, "shared");
    REQUIRE(shared != nullptr);
    CHECK(shared->origin == coding_agent::tui::ThemeResourceOrigin::Explicit);
    const coding_agent::tui::RgbThemeColor explicit_accent{.red = 51, .green = 51, .blue = 51};
    CHECK(accent_of(shared->theme) == explicit_accent);
    CHECK(catalog->initial_theme.name == "shared");
    CHECK(catalog->initial_theme_origin == coding_agent::tui::ThemeResourceOrigin::Explicit);
    CHECK(std::any_of(catalog->diagnostics.begin(), catalog->diagnostics.end(), [](const auto& diagnostic) {
        return diagnostic.code == "duplicate_theme_skipped";
    }));
}

TEST_CASE(
    "Theme catalog consumes only project themes admitted by Project Trust",
    "[coding_agent][theme][issue56]") {
    tests::TempWorkspace workspace;
    workspace.write(".pi/themes/dark.json", fixture_theme("dark", "#010203"));
    auto fs = harness::WorkspaceFileSystem::create(workspace.path());
    REQUIRE(fs);
    coding_agent::ProjectTrustStore trust_store(workspace.path() / "trust.json");

    const auto load_project_catalog = [&](coding_agent::DefaultProjectTrust trust) {
        coding_agent::ProjectResourceLoadingRequest resource_request;
        resource_request.workspace = workspace.path();
        resource_request.default_project_trust = trust;
        resource_request.theme_resources_enabled = true;
        auto resources = coding_agent::load_project_resources(*fs, trust_store, std::move(resource_request));

        coding_agent::tui::ThemeCatalogRequest catalog_request;
        catalog_request.user_active_theme = "dark";
        for (auto& theme : resources.resources.project_themes) {
            catalog_request.trusted_project_themes.push_back({
                .label = std::move(theme.path),
                .json = std::move(theme.json),
            });
        }
        return coding_agent::tui::load_theme_catalog(std::move(catalog_request));
    };

    const auto untrusted = load_project_catalog(coding_agent::DefaultProjectTrust::Never);
    REQUIRE(untrusted);
    CHECK(untrusted->initial_theme == coding_agent::tui::builtin_dark_theme());
    CHECK(untrusted->initial_theme_origin == coding_agent::tui::ThemeResourceOrigin::Builtin);

    const auto trusted = load_project_catalog(coding_agent::DefaultProjectTrust::Always);
    REQUIRE(trusted);
    CHECK(trusted->initial_theme_origin == coding_agent::tui::ThemeResourceOrigin::Project);
    const coding_agent::tui::RgbThemeColor project_accent{.red = 1, .green = 2, .blue = 3};
    CHECK(accent_of(trusted->initial_theme) == project_accent);
}

TEST_CASE(
    "Theme catalog separates appearance-selected built-ins from same-name overrides",
    "[coding_agent][theme][issue56]") {
    tests::TempWorkspace config;
    config.write("themes/light.json", fixture_theme("light", "#010203"));

    coding_agent::tui::ThemeCatalogRequest detected_request;
    detected_request.agent_config_directory = config.path();
    detected_request.terminal_capabilities.appearance = tui::TerminalAppearance::Light;
    const auto detected = coding_agent::tui::load_theme_catalog(std::move(detected_request));

    REQUIRE(detected);
    CHECK(detected->initial_theme == coding_agent::tui::builtin_light_theme());
    CHECK(detected->initial_theme_origin == coding_agent::tui::ThemeResourceOrigin::Builtin);
    const auto* effective_light = find_theme(*detected, "light");
    REQUIRE(effective_light != nullptr);
    CHECK(effective_light->origin == coding_agent::tui::ThemeResourceOrigin::Global);

    coding_agent::tui::ThemeCatalogRequest configured_request;
    configured_request.agent_config_directory = config.path();
    configured_request.user_active_theme = "light";
    configured_request.terminal_capabilities.appearance = tui::TerminalAppearance::Dark;
    const auto configured = coding_agent::tui::load_theme_catalog(std::move(configured_request));

    REQUIRE(configured);
    CHECK(configured->initial_theme_origin == coding_agent::tui::ThemeResourceOrigin::Global);
    const coding_agent::tui::RgbThemeColor global_accent{.red = 1, .green = 2, .blue = 3};
    CHECK(accent_of(configured->initial_theme) == global_accent);
}

TEST_CASE(
    "Theme catalog fails unavailable explicit selection and warns for unavailable User Settings",
    "[coding_agent][theme][issue56]") {
    coding_agent::tui::ThemeCatalogRequest explicit_request;
    explicit_request.explicit_active_theme = "missing";
    const auto explicit_result = coding_agent::tui::load_theme_catalog(std::move(explicit_request));
    REQUIRE_FALSE(explicit_result);
    CHECK(explicit_result.error().message.find("explicit active theme") != std::string::npos);

    coding_agent::tui::ThemeCatalogRequest settings_request;
    settings_request.user_active_theme = "missing";
    settings_request.terminal_capabilities.appearance = tui::TerminalAppearance::Light;
    const auto settings_result = coding_agent::tui::load_theme_catalog(std::move(settings_request));
    REQUIRE(settings_result);
    CHECK(settings_result->initial_theme == coding_agent::tui::builtin_light_theme());
    REQUIRE_FALSE(settings_result->diagnostics.empty());
    CHECK(settings_result->diagnostics.front().code == "configured_theme_unavailable");
}

TEST_CASE(
    "Theme catalog makes explicit failures fatal and bounds automatic diagnostics",
    "[coding_agent][theme][issue56]") {
    tests::TempWorkspace config;
    for (int index = 0; index < 70; ++index) {
        config.write(
            "themes/bad-" + std::to_string(index) + ".json",
            std::string{"{\"name\":\"sk-abcdefghijklmnopqrstuvwxyz123456\","} + std::string(3000, 'x'));
    }

    coding_agent::tui::ThemeCatalogRequest automatic_request;
    automatic_request.agent_config_directory = config.path();
    const auto automatic = coding_agent::tui::load_theme_catalog(std::move(automatic_request));
    REQUIRE(automatic);
    CHECK(automatic->diagnostics.size() == 64);
    CHECK(automatic->diagnostics.back().code == "diagnostics_truncated");
    for (const auto& diagnostic : automatic->diagnostics) {
        CHECK(diagnostic.message.size() <= 1024);
        if (diagnostic.path) CHECK(diagnostic.path->size() <= 1024);
        CHECK(diagnostic.message.find("sk-abcdefghijklmnopqrstuvwxyz123456") == std::string::npos);
    }

    tests::TempWorkspace explicit_base;
    explicit_base.write("bad.json", "{not json");
    coding_agent::tui::ThemeCatalogRequest explicit_request;
    explicit_request.explicit_path_base = explicit_base.path();
    explicit_request.explicit_paths = {"bad.json"};
    const auto explicit_result = coding_agent::tui::load_theme_catalog(std::move(explicit_request));
    REQUIRE_FALSE(explicit_result);
    CHECK(explicit_result.error().message.size() <= coding_agent::kMaxPresentationPayloadBytes);
}

TEST_CASE(
    "Theme controller preserves the active palette when settings persistence fails",
    "[coding_agent][theme][settings][issue56]") {
    coding_agent::tui::ThemeCatalogRequest request;
    request.explicit_active_theme = "dark";
    auto catalog = coding_agent::tui::load_theme_catalog(std::move(request));
    REQUIRE(catalog);

    tui::VirtualTerminal terminal;
    tui::Tui root(terminal);
    auto base = std::make_unique<InvalidatingText>();
    auto* base_pointer = base.get();
    REQUIRE(root.add_child(std::move(base)));
    coding_agent::tui::ThemeController controller(
        std::move(*catalog),
        root,
        tui::TerminalColorCapability::TrueColor,
        [](std::string_view) -> util::ExpectedVoid {
            return std::unexpected(util::make_error(util::ErrorCode::Workspace, "settings write failed"));
        });

    const auto selected = controller.select_theme("light");

    REQUIRE_FALSE(selected);
    CHECK(controller.active_theme_name() == "dark");
    CHECK(base_pointer->invalidations == 0);
}

TEST_CASE(
    "Theme settings overlay lists effective themes selects one and invalidates the root",
    "[coding_agent][theme][settings][issue56]") {
    tests::TempWorkspace config;
    config.write("themes/solarized.json", fixture_theme("solarized", "#abcdef"));
    config.write("settings.json", R"({"theme":"dark","future":true})");
    auto settings_manager = coding_agent::SettingsManager::create(
        /* cwd */ {}, config.path(), /* project_trusted */ false);
    REQUIRE(settings_manager.errors().empty());

    coding_agent::tui::ThemeCatalogRequest request;
    request.agent_config_directory = config.path();
    request.user_active_theme = settings_manager.global_settings().theme;
    request.terminal_capabilities.color = tui::TerminalColorCapability::TrueColor;
    auto catalog = coding_agent::tui::load_theme_catalog(std::move(request));
    REQUIRE(catalog);

    tui::VirtualTerminal terminal({.columns = 60, .rows = 18});
    tui::Tui root(terminal);
    auto base = std::make_unique<InvalidatingText>();
    auto* base_pointer = base.get();
    REQUIRE(root.add_child(std::move(base)));
    coding_agent::tui::ThemeController controller(
        std::move(*catalog),
        root,
        tui::TerminalColorCapability::TrueColor,
        [manager = std::move(settings_manager)](std::string_view name) mutable {
            return manager.set_theme(coding_agent::SettingsScope::Global, name);
        });
    auto overlay = coding_agent::tui::make_theme_settings_overlay(
        controller,
        tui::default_tui_keybindings());
    REQUIRE(overlay);
    auto* overlay_pointer = overlay->get();
    REQUIRE(root.add_overlay(std::move(*overlay)));
    REQUIRE(root.start());
    REQUIRE(root.set_focus(overlay_pointer));
    REQUIRE(root.render());

    REQUIRE(terminal.inject_input("\r"));
    REQUIRE(root.render());
    const auto submenu_screen = terminal.screen();
    CHECK(std::any_of(submenu_screen.begin(), submenu_screen.end(), [](const auto& line) {
        return line.find("dark") != std::string::npos;
    }));
    CHECK(std::any_of(submenu_screen.begin(), submenu_screen.end(), [](const auto& line) {
        return line.find("light") != std::string::npos;
    }));
    CHECK(std::any_of(submenu_screen.begin(), submenu_screen.end(), [](const auto& line) {
        return line.find("solarized") != std::string::npos;
    }));

    REQUIRE(terminal.inject_input("\x1b[B"));
    REQUIRE(terminal.inject_input("\r"));
    CHECK(controller.active_theme_name() == "light");
    auto saved_manager = coding_agent::SettingsManager::create(
        /* cwd */ {}, config.path(), /* project_trusted */ false);
    REQUIRE(saved_manager.errors().empty());
    CHECK(saved_manager.global_settings().theme == "light");
    CHECK(config.read("settings.json").find("future") != std::string::npos);
    CHECK(base_pointer->invalidations > 0);
    CHECK(controller.live_theme().foreground(coding_agent::tui::ThemeToken::Accent, "x") ==
        "\x1b[38;2;90;128;128mx\x1b[39m");
}

// The pi `InteractiveThemeController` subset (G5): env-only COLORFGBG
// default detection, `resolveThemeSetting` with slash automatic-pair values
// read as unset, `applyThemeName` fallback semantics with the verbatim
// failure message, `applyFromSettings` (persist on high confidence),
// in-memory `preview`, registered themes + `discover_themes` dedupe with pi's
// collision diagnostics, and the boot theme init (`init_boot_theme`).

#include "coding_agent/tui/ThemeController.hpp"
#include "support/EnvVarGuard.hpp"
#include "support/ThemeFixture.hpp"
#include "support/TempWorkspace.hpp"

#include <cch/tui/Text.hpp>
#include <cch/tui/Tui.hpp>
#include <cch/tui/VirtualTerminal.hpp>

#include "../../../third_party/catch2/catch_test_macros.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace cch;

namespace {

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

struct ControllerHarness {
    tui::VirtualTerminal terminal;
    tui::Tui root;
    std::unique_ptr<InvalidatingText> base;
    InvalidatingText* base_pointer{nullptr};
    std::vector<std::string> errors;
    std::size_t changed{0};
    std::vector<std::string> committed;

    ControllerHarness()
        : root(terminal) {
        base = std::make_unique<InvalidatingText>();
        base_pointer = base.get();
        REQUIRE(root.add_child(std::move(base)));
    }
};

[[nodiscard]] coding_agent::tui::ThemeController make_controller(
    ControllerHarness& harness,
    std::filesystem::path custom_themes_dir = {},
    std::vector<coding_agent::tui::RegisteredTheme> registered = {},
    std::optional<std::string> theme_setting = std::nullopt,
    coding_agent::tui::ThemeSettingCommitter committer = {}) {
    return coding_agent::tui::ThemeController(
        std::move(custom_themes_dir),
        std::move(registered),
        [theme_setting = std::move(theme_setting)]() mutable { return theme_setting; },
        std::move(committer),
        tui::TerminalColorCapability::TrueColor,
        harness.root,
        [&harness](std::string message) { harness.errors.push_back(std::move(message)); },
        [&harness] { ++harness.changed; });
}

[[nodiscard]] coding_agent::tui::RgbThemeColor accent_of(
    const coding_agent::tui::ResolvedTheme& theme) {
    const auto* color = std::get_if<coding_agent::tui::RgbThemeColor>(
        &coding_agent::tui::color_for(theme, coding_agent::tui::ThemeToken::Accent));
    REQUIRE(color != nullptr);
    return *color;
}

} // namespace

TEST_CASE(
    "COLORFGBG detection classifies the background index by luminance",
    "[coding_agent][theme][controller][issue415]") {
    using coding_agent::tui::TerminalTheme;
    const auto light = coding_agent::tui::terminal_theme_from_colorfgbg("0;15");
    CHECK(light.theme == TerminalTheme::Light);
    CHECK(light.high_confidence);

    const auto dark = coding_agent::tui::terminal_theme_from_colorfgbg("15;0");
    CHECK(dark.theme == TerminalTheme::Dark);
    CHECK(dark.high_confidence);

    // The last COLORFGBG field is the background (pi test pin).
    const auto last_field = coding_agent::tui::terminal_theme_from_colorfgbg("0;7;15");
    CHECK(last_field.theme == TerminalTheme::Light);
    CHECK(last_field.high_confidence);

    const auto fallback = coding_agent::tui::terminal_theme_from_colorfgbg("");
    CHECK(fallback.theme == TerminalTheme::Dark);
    CHECK_FALSE(fallback.high_confidence);
    CHECK(coding_agent::tui::terminal_theme_from_colorfgbg("abc").high_confidence == false);
}

TEST_CASE(
    "resolve_theme_setting reads slash automatic-pair values as unset",
    "[coding_agent][theme][controller][issue415]") {
    using coding_agent::tui::TerminalTheme;
    using coding_agent::tui::resolve_theme_setting;

    CHECK(resolve_theme_setting(std::optional<std::string>{"dark"}, TerminalTheme::Light) ==
        std::optional<std::string>{"dark"});
    // The automatic `light/dark` pair is absent: slash values read as unset.
    CHECK_FALSE(resolve_theme_setting(
        std::optional<std::string>{"light/dark"}, TerminalTheme::Dark).has_value());
    CHECK_FALSE(resolve_theme_setting(
        std::optional<std::string>{"light/dark/extra"}, TerminalTheme::Dark).has_value());
    CHECK_FALSE(resolve_theme_setting(
        std::optional<std::string>{"/dark"}, TerminalTheme::Dark).has_value());
    CHECK_FALSE(resolve_theme_setting(
        std::optional<std::string>{}, TerminalTheme::Dark).has_value());
}

TEST_CASE(
    "init_boot_theme resolves the setting or the env default with silent dark fallback",
    "[coding_agent][theme][controller][issue415]") {
    using coding_agent::tui::init_boot_theme;
    tests::TempWorkspace config;
    config.write("themes/solarized.json", tests::fixture_theme("solarized", "#abcdef"));

    {
        tests::EnvVarGuard env("COLORFGBG", "0;15");
        CHECK(init_boot_theme({}, std::nullopt) == coding_agent::tui::builtin_light_theme());
    }
    {
        tests::EnvVarGuard env("COLORFGBG", "15;0");
        CHECK(init_boot_theme({}, std::nullopt) == coding_agent::tui::builtin_dark_theme());
    }
    // A defined setting wins over the env default.
    CHECK(init_boot_theme({}, std::optional<std::string>{"dark"}) ==
        coding_agent::tui::builtin_dark_theme());
    // A custom-directory theme loads by name.
    const auto custom = init_boot_theme(config.path(), std::optional<std::string>{"solarized"});
    REQUIRE(custom.name == "solarized");
    const coding_agent::tui::RgbThemeColor solarized_accent{.red = 0xab, .green = 0xcd, .blue = 0xef};
    CHECK(accent_of(custom) == solarized_accent);
    // An unknown name falls back to dark silently.
    CHECK(init_boot_theme({}, std::optional<std::string>{"missing"}) ==
        coding_agent::tui::builtin_dark_theme());
}

TEST_CASE(
    "theme controller boots from the settings theme or the env default",
    "[coding_agent][theme][controller][issue415]") {
    ControllerHarness harness;

    {
        tests::EnvVarGuard env("COLORFGBG", std::nullopt);
        auto controller = make_controller(harness, {}, {}, std::optional<std::string>{"dark"});
        CHECK(controller.active_theme_name() == "dark");
        CHECK(controller.terminal_theme() == coding_agent::tui::TerminalTheme::Dark);
        CHECK(harness.errors.empty());
        CHECK(harness.changed == 0);
    }
    {
        tests::EnvVarGuard env("COLORFGBG", "0;15");
        auto controller = make_controller(harness);
        CHECK(controller.active_theme_name() == "light");
        CHECK(controller.live_theme().foreground(coding_agent::tui::ThemeToken::Accent, "x") ==
            "\x1b[38;2;90;128;128mx\x1b[39m");
    }
    // A failing settings theme falls back to dark silently at boot.
    {
        tests::EnvVarGuard env("COLORFGBG", std::nullopt);
        auto controller = make_controller(harness, {}, {}, std::optional<std::string>{"missing"});
        CHECK(controller.active_theme_name() == "dark");
        CHECK(harness.errors.empty());
    }
}

TEST_CASE(
    "theme controller registered themes apply by name and re-register on set_registered_themes",
    "[coding_agent][theme][controller][issue415]") {
    ControllerHarness harness;
    tests::EnvVarGuard env("COLORFGBG", std::nullopt);
    std::vector<coding_agent::tui::RegisteredTheme> registered{{
        .theme = *coding_agent::tui::parse_theme_json("solarized.json", tests::fixture_theme("solarized", "#abcdef")),
        .source_path = "solarized.json",
    }};
    auto controller = make_controller(harness, {}, std::move(registered));

    CHECK(controller.set_theme_name("solarized", false));
    CHECK(controller.active_theme_name() == "solarized");
    CHECK(harness.changed == 1);
    CHECK(harness.base_pointer->invalidations == 1);
    CHECK(controller.live_theme().foreground(coding_agent::tui::ThemeToken::Accent, "x") ==
        "\x1b[38;2;171;205;239mx\x1b[39m");

    // pi `setRegisteredThemes` replaces the map (reload re-registration).
    controller.set_registered_themes({});
    CHECK_FALSE(controller.set_theme_name("solarized", false));
    CHECK(controller.active_theme_name() == "dark");
    CHECK(harness.changed == 2);
}

TEST_CASE(
    "apply_theme_name failure falls back to dark with the verbatim message",
    "[coding_agent][theme][controller][issue415]") {
    ControllerHarness harness;
    tests::EnvVarGuard env("COLORFGBG", std::nullopt);
    auto controller = make_controller(harness, {}, {}, std::optional<std::string>{"dark"});

    const auto failed = controller.set_theme_name("missing", /* show_error */ true);

    CHECK_FALSE(failed);
    CHECK(controller.active_theme_name() == "dark");
    // pi applyThemeName verbatim:
    // `Failed to load theme "<name>": <error>\nFell back to dark theme.`
    REQUIRE(harness.errors.size() == 1);
    CHECK(harness.errors[0] ==
        "Failed to load theme \"missing\": Theme not found: missing\nFell back to dark theme.");
    CHECK(harness.changed == 1);

    // Without show_error the fallback stays silent.
    harness.errors.clear();
    CHECK_FALSE(controller.set_theme_name("missing", false));
    CHECK(harness.errors.empty());
}

TEST_CASE(
    "apply_from_settings applies a defined setting and persists a confident default",
    "[coding_agent][theme][controller][issue415]") {
    ControllerHarness harness;

    {
        tests::EnvVarGuard env("COLORFGBG", std::nullopt);
        std::vector<std::string> committed;
        auto controller = make_controller(
            harness, {}, {}, std::optional<std::string>{"dark"},
            [&committed](std::string_view name) {
                committed.emplace_back(name);
                return util::ExpectedVoid{};
            });
        controller.apply_from_settings();
        CHECK(controller.active_theme_name() == "dark");
        CHECK(committed.empty());
    }
    {
        // Unset setting + high-confidence COLORFGBG detection: the detected
        // default applies and persists to the global scope (pi
        // `settingsManager.setTheme(detection.theme)` + flush).
        tests::EnvVarGuard env("COLORFGBG", "0;15");
        std::vector<std::string> committed;
        auto controller = make_controller(
            harness, {}, {}, std::nullopt,
            [&committed](std::string_view name) {
                committed.emplace_back(name);
                return util::ExpectedVoid{};
            });
        controller.apply_from_settings();
        CHECK(controller.active_theme_name() == "light");
        REQUIRE(committed.size() == 1);
        CHECK(committed[0] == "light");
    }
    {
        // No COLORFGBG: the dark fallback applies without persisting.
        tests::EnvVarGuard env("COLORFGBG", std::nullopt);
        std::size_t commits = 0;
        auto controller = make_controller(
            harness, {}, {}, std::nullopt,
            [&commits](std::string_view) {
                ++commits;
                return util::ExpectedVoid{};
            });
        controller.apply_from_settings();
        CHECK(controller.active_theme_name() == "dark");
        CHECK(commits == 0);
    }
    {
        // Slash automatic-pair settings read as unset and boot identically:
        // the env detection applies and persists on high confidence (pi
        // `applyFromSettings` unset branch).
        tests::EnvVarGuard env("COLORFGBG", "15;0");
        std::vector<std::string> committed;
        auto controller = make_controller(
            harness, {}, {}, std::optional<std::string>{"light/dark"},
            [&committed](std::string_view name) {
                committed.emplace_back(name);
                return util::ExpectedVoid{};
            });
        controller.apply_from_settings();
        CHECK(controller.active_theme_name() == "dark");
        REQUIRE(committed.size() == 1);
        CHECK(committed[0] == "dark");
    }
}

TEST_CASE(
    "preview applies in memory without committing or changing the active name",
    "[coding_agent][theme][controller][issue415]") {
    ControllerHarness harness;
    tests::EnvVarGuard env("COLORFGBG", std::nullopt);
    std::vector<coding_agent::tui::RegisteredTheme> registered{{
        .theme = *coding_agent::tui::parse_theme_json("solarized.json", tests::fixture_theme("solarized", "#abcdef")),
        .source_path = "solarized.json",
    }};
    auto controller = make_controller(harness, {}, std::move(registered));

    controller.preview("solarized");

    // The palette changed in memory; the active name and the change sink
    // did not (pi preview: `setTheme` + invalidate, no `applyThemeName`).
    CHECK(controller.active_theme_name() == "dark");
    CHECK(harness.changed == 0);
    CHECK(harness.base_pointer->invalidations == 1);
    CHECK(controller.live_theme().foreground(coding_agent::tui::ThemeToken::Accent, "x") ==
        "\x1b[38;2;171;205;239mx\x1b[39m");

    // A slash setting preview resolves to the active theme (pi preview
    // `resolveThemeSetting(...) ?? activeThemeName`).
    const auto before = harness.base_pointer->invalidations;
    controller.preview("light/dark");
    CHECK(harness.base_pointer->invalidations == before + 1);
    CHECK(controller.active_theme_name() == "dark");
}

TEST_CASE(
    "available_theme_names lists builtins custom directory and registered sorted",
    "[coding_agent][theme][controller][issue415]") {
    ControllerHarness harness;
    tests::TempWorkspace config;
    config.write("themes/solarized.json", tests::fixture_theme("solarized", "#abcdef"));
    config.write("themes/broken.json", "{not json");
    std::vector<coding_agent::tui::RegisteredTheme> registered{{
        .theme = *coding_agent::tui::parse_theme_json("registered.json", tests::fixture_theme("registered", "#010203")),
        .source_path = "registered.json",
    }};

    auto controller = make_controller(harness, config.path() / "themes", std::move(registered));

    // pi `getAvailableThemes`: builtins + custom directory (invalid files
    // ignored) + registered, deduped and sorted.
    const auto names = controller.available_theme_names();
    REQUIRE(names.size() == 4);
    CHECK(names[0] == "dark");
    CHECK(names[1] == "light");
    CHECK(names[2] == "registered");
    CHECK(names[3] == "solarized");
}

TEST_CASE(
    "discover_themes parses documents and dedupes with pi collision diagnostics",
    "[coding_agent][theme][controller][issue415]") {
    std::vector<coding_agent::LoadedThemeResource> documents{{
        .path = ".pi/themes/project.json",
        .json = tests::fixture_theme("shared", "#111111"),
        .scope = coding_agent::SourceScope::Project,
    }, {
        .path = "/home/user/.pi/agent/themes/user.json",
        .json = tests::fixture_theme("user-theme", "#222222"),
        .scope = coding_agent::SourceScope::User,
    }, {
        .path = "shared.json",
        .json = tests::fixture_theme("shared", "#333333"),
        .scope = coding_agent::SourceScope::Temporary,
    }, {
        .path = "broken.json",
        .json = "{not json",
        .scope = coding_agent::SourceScope::Temporary,
    }};

    auto discovery = coding_agent::tui::discover_themes(std::move(documents));

    // First-wins dedupe in load order (discovered before explicit): the
    // project theme wins and the explicit duplicate loses with pi's
    // collision diagnostic carrying the winner/loser paths.
    REQUIRE(discovery.themes.size() == 2);
    CHECK(discovery.themes[0].theme.name == "shared");
    CHECK(discovery.themes[0].source_path == std::filesystem::path{".pi/themes/project.json"});
    const coding_agent::tui::RgbThemeColor project_accent{.red = 0x11, .green = 0x11, .blue = 0x11};
    CHECK(accent_of(discovery.themes[0].theme) == project_accent);
    CHECK(discovery.themes[1].theme.name == "user-theme");
    CHECK(discovery.themes[1].source_path ==
        std::filesystem::path{"/home/user/.pi/agent/themes/user.json"});

    const auto collision = std::find_if(
        discovery.diagnostics.begin(),
        discovery.diagnostics.end(),
        [](const auto& diagnostic) {
            return diagnostic.type == coding_agent::ResourceDiagnosticType::Collision;
        });
    REQUIRE(collision != discovery.diagnostics.end());
    CHECK(collision->message == "name \"shared\" collision");
    REQUIRE(collision->collision.has_value());
    CHECK(collision->collision->resource_type == coding_agent::ResourceCollisionResourceType::Theme);
    CHECK(collision->collision->name == "shared");
    CHECK(collision->collision->winner_path == ".pi/themes/project.json");
    CHECK(collision->collision->loser_path == "shared.json");

    // The broken document is a warning with its path, like pi's
    // `loadThemeFromFile` catch.
    const auto parse_warning = std::find_if(
        discovery.diagnostics.begin(),
        discovery.diagnostics.end(),
        [](const auto& diagnostic) {
            return diagnostic.type == coding_agent::ResourceDiagnosticType::Warning;
        });
    REQUIRE(parse_warning != discovery.diagnostics.end());
    CHECK(parse_warning->path == std::optional<std::string>{"broken.json"});
}

TEST_CASE(
    "theme controller preview failure falls back to dark without invalidating",
    "[coding_agent][theme][controller][issue415]") {
    ControllerHarness harness;
    tests::EnvVarGuard env("COLORFGBG", std::nullopt);
    auto controller = make_controller(harness, {}, {}, std::optional<std::string>{"dark"});

    controller.preview("missing");

    // pi `setTheme` failure replaces the palette with dark silently; the
    // preview invalidates only on success.
    CHECK(harness.base_pointer->invalidations == 0);
    CHECK(controller.active_theme_name() == "dark");
}

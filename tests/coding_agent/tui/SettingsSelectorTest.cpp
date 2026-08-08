// The settings selector (pi `settings-selector.ts` subset): renders the #327
// settings subset plus the two graduated render settings with pi's labels and
// descriptions, cycles value items on confirm, opens the thinking-level
// submenu with pi's per-level descriptions, opens the optional single-mode
// Theme submenu through the injected factory, and cancels on Escape/Ctrl+C.

#include "coding_agent/tui/SettingsSelector.hpp"
#include "coding_agent/tui/Theme.hpp"
#include "support/TempWorkspace.hpp"

#include <cch/tui/Keybindings.hpp>

#include "../../../third_party/catch2/catch_test_macros.hpp"

#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace cch;

namespace {

[[nodiscard]] std::shared_ptr<const tui::KeybindingRegistry> test_keybindings() {
    tui::KeybindingResolutionRequest request;
    request.definitions = tui::builtin_tui_keybinding_definitions();
    auto resolved = tui::resolve_keybindings(std::move(request));
    REQUIRE(resolved);
    return resolved->registry;
}

[[nodiscard]] coding_agent::tui::LiveTheme test_theme() {
    return coding_agent::tui::LiveTheme(
        coding_agent::tui::builtin_dark_theme(),
        tui::TerminalColorCapability::TrueColor);
}

[[nodiscard]] std::string strip_ansi(std::string_view text) {
    std::string stripped;
    stripped.reserve(text.size());
    for (std::size_t index = 0; index < text.size();) {
        if (text[index] == '\x1b' && index + 1 < text.size() && text[index + 1] == '[') {
            index += 2;
            while (index < text.size() && !(text[index] >= '@' && text[index] <= '~')) ++index;
            if (index < text.size()) ++index;
            continue;
        }
        stripped.push_back(text[index]);
        ++index;
    }
    return stripped;
}

[[nodiscard]] std::string join_lines(const std::vector<std::string>& lines) {
    std::string text;
    for (const auto& line : lines) {
        text.append(strip_ansi(line));
        text.push_back('\n');
    }
    return text;
}

[[nodiscard]] std::string render_screen(coding_agent::tui::SettingsSelectorComponent& selector) {
    auto rendered = selector.render(80);
    REQUIRE(rendered);
    return join_lines(rendered->lines);
}

} // namespace

TEST_CASE(
    "SettingsSelector renders the #327 subset items with pi labels and resolved values",
    "[coding_agent][tui][settings-selector][issue408]") {
    auto theme = test_theme();
    coding_agent::tui::SettingsSelectorConfig config;
    config.hide_thinking_block = true;
    config.output_pad = 0;
    config.thinking_level = "high";
    config.available_thinking_levels = {"off", "low", "high"};
    config.default_project_trust = coding_agent::DefaultProjectTrust::Always;
    config.current_theme = "dark";
    coding_agent::tui::SettingsSelectorCallbacks callbacks;
    callbacks.theme_submenu_factory = [](const tui::SettingItem&, tui::SettingsSubmenuDoneSink) {
        return std::unique_ptr<tui::Component>{};
    };

    coding_agent::tui::SettingsSelectorComponent selector(
        theme,
        test_keybindings(),
        config,
        std::move(callbacks));

    const auto screen = render_screen(selector);
    // pi settings-selector.ts item order for the C++ subset: output-padding,
    // hide-thinking, default-project-trust, thinking, theme.
    const auto output_padding = screen.find("Output padding");
    const auto hide_thinking = screen.find("Hide thinking");
    const auto project_trust = screen.find("Default project trust");
    const auto thinking = screen.find("Thinking level");
    const auto theme_item = screen.find("Theme");
    REQUIRE(output_padding != std::string::npos);
    REQUIRE(hide_thinking != std::string::npos);
    REQUIRE(project_trust != std::string::npos);
    REQUIRE(thinking != std::string::npos);
    REQUIRE(theme_item != std::string::npos);
    CHECK(output_padding < hide_thinking);
    CHECK(hide_thinking < project_trust);
    CHECK(project_trust < thinking);
    CHECK(thinking < theme_item);
    // The selected item's description renders (pi verbatim wording); the
    // other descriptions render as their rows are selected.
    CHECK(screen.find("Horizontal padding for user messages, assistant messages, and thinking") !=
        std::string::npos);
    selector.handle_input(tui::KeyEvent{.key = "down"});
    CHECK(render_screen(selector).find("Hide thinking blocks in assistant responses") !=
        std::string::npos);
    selector.handle_input(tui::KeyEvent{.key = "down"});
    CHECK(render_screen(selector).find("Fallback behavior when no extension") !=
        std::string::npos);
    selector.handle_input(tui::KeyEvent{.key = "down"});
    CHECK(render_screen(selector).find("Reasoning depth for thinking-capable models") !=
        std::string::npos);
    selector.handle_input(tui::KeyEvent{.key = "down"});
    CHECK(render_screen(selector).find("Color theme for the interface") !=
        std::string::npos);
    // Resolved current values.
    const auto resolved = render_screen(selector);
    CHECK(resolved.find("true") != std::string::npos);
    CHECK(resolved.find("0") != std::string::npos);
    CHECK(resolved.find("Always trust") != std::string::npos);
    CHECK(resolved.find("high") != std::string::npos);
    CHECK(resolved.find("dark") != std::string::npos);
}

TEST_CASE(
    "SettingsSelector cycles value items and fires the change sinks",
    "[coding_agent][tui][settings-selector][issue408]") {
    auto theme = test_theme();
    coding_agent::tui::SettingsSelectorConfig config;
    config.output_pad = 1;
    config.default_project_trust = coding_agent::DefaultProjectTrust::Ask;
    std::optional<bool> hide_change;
    std::optional<std::size_t> pad_change;
    std::optional<coding_agent::DefaultProjectTrust> trust_change;
    std::size_t cancellations = 0;
    coding_agent::tui::SettingsSelectorCallbacks callbacks;
    callbacks.on_hide_thinking_block_change = [&hide_change](bool hidden) { hide_change = hidden; };
    callbacks.on_output_pad_change = [&pad_change](std::size_t padding) { pad_change = padding; };
    callbacks.on_default_project_trust_change =
        [&trust_change](coding_agent::DefaultProjectTrust trust) { trust_change = trust; };
    callbacks.on_cancel = [&cancellations] { ++cancellations; };

    coding_agent::tui::SettingsSelectorComponent selector(
        theme,
        test_keybindings(),
        config,
        std::move(callbacks));

    // Confirm on the first item (output-padding) cycles 1 → 0.
    selector.handle_input(tui::KeyEvent{.key = "enter"});
    REQUIRE(pad_change.has_value());
    CHECK(*pad_change == 0);

    // Down to hide-thinking; confirm cycles false → true.
    selector.handle_input(tui::KeyEvent{.key = "down"});
    selector.handle_input(tui::KeyEvent{.key = "enter"});
    REQUIRE(hide_change.has_value());
    CHECK(*hide_change == true);

    // Down to default-project-trust; confirm cycles Ask → Always trust.
    selector.handle_input(tui::KeyEvent{.key = "down"});
    selector.handle_input(tui::KeyEvent{.key = "enter"});
    REQUIRE(trust_change.has_value());
    CHECK(*trust_change == coding_agent::DefaultProjectTrust::Always);

    // Cancel fires the cancel sink.
    selector.handle_input(tui::KeyEvent{.key = "escape"});
    CHECK(cancellations == 1);
}

TEST_CASE(
    "SettingsSelector thinking submenu lists the available levels with pi descriptions",
    "[coding_agent][tui][settings-selector][issue408]") {
    auto theme = test_theme();
    coding_agent::tui::SettingsSelectorConfig config;
    config.thinking_level = "low";
    config.available_thinking_levels = {"off", "low", "high", "xhigh"};
    std::optional<std::string> level_change;
    coding_agent::tui::SettingsSelectorCallbacks callbacks;
    callbacks.on_thinking_level_change = [&level_change](std::string level) { level_change = std::move(level); };

    coding_agent::tui::SettingsSelectorComponent selector(
        theme,
        test_keybindings(),
        config,
        std::move(callbacks));

    // Navigate to the thinking item (index 3) and open the submenu.
    selector.handle_input(tui::KeyEvent{.key = "down"});
    selector.handle_input(tui::KeyEvent{.key = "down"});
    selector.handle_input(tui::KeyEvent{.key = "down"});
    selector.handle_input(tui::KeyEvent{.key = "enter"});

    const auto screen = render_screen(selector);
    CHECK(screen.find("No reasoning") != std::string::npos);
    CHECK(screen.find("Light reasoning (~2k tokens)") != std::string::npos);
    CHECK(screen.find("Deep reasoning (~16k tokens)") != std::string::npos);
    CHECK(screen.find("Extra-high reasoning (~32k tokens)") != std::string::npos);

    // The current level is pre-selected; confirming selects it.
    selector.handle_input(tui::KeyEvent{.key = "enter"});
    REQUIRE(level_change.has_value());
    CHECK(*level_change == "low");
}

TEST_CASE(
    "SettingsSelector Theme item opens the injected theme submenu factory",
    "[coding_agent][tui][settings-selector][issue408]") {
    auto theme = test_theme();
    coding_agent::tui::SettingsSelectorConfig config;
    config.current_theme = "dark";
    std::size_t factory_calls = 0;
    coding_agent::tui::SettingsSelectorCallbacks callbacks;
    callbacks.theme_submenu_factory =
        [&factory_calls](const tui::SettingItem& item, tui::SettingsSubmenuDoneSink done) {
            ++factory_calls;
            CHECK(item.id == "theme");
            auto list = std::make_unique<tui::SelectList>(
                std::vector<tui::SelectItem>{{.value = "dark", .label = "dark"}},
                tui::SelectListOptions{
                    .on_select = [done = std::move(done)](const tui::SelectItem& selected) mutable {
                        done(selected.value);
                    },
                });
            return list;
        };

    coding_agent::tui::SettingsSelectorComponent selector(
        theme,
        test_keybindings(),
        config,
        std::move(callbacks));

    // Navigate to the theme item (last, index 4) and confirm.
    for (int step = 0; step < 4; ++step) {
        selector.handle_input(tui::KeyEvent{.key = "down"});
    }
    selector.handle_input(tui::KeyEvent{.key = "enter"});
    CHECK(factory_calls == 1);
    const auto screen = render_screen(selector);
    CHECK(screen.find("dark") != std::string::npos);
}

TEST_CASE(
    "SettingsSelector search filters the subset items",
    "[coding_agent][tui][settings-selector][issue408]") {
    auto theme = test_theme();
    coding_agent::tui::SettingsSelectorComponent selector(
        theme,
        test_keybindings(),
        coding_agent::tui::SettingsSelectorConfig{},
        coding_agent::tui::SettingsSelectorCallbacks{});

    // Typing filters by label (pi settings-list.ts search).
    selector.handle_input(tui::KeyEvent{.key = "t"});
    selector.handle_input(tui::KeyEvent{.key = "h"});
    const auto screen = render_screen(selector);
    CHECK(screen.find("Thinking level") != std::string::npos);
    CHECK(screen.find("Hide thinking") != std::string::npos);
    CHECK(screen.find("Output padding") == std::string::npos);
    CHECK(screen.find("Default project trust") == std::string::npos);
    CHECK(screen.find("Theme") == std::string::npos);
}

TEST_CASE(
    "SettingsSelector omits the Theme item without a wired submenu factory",
    "[coding_agent][tui][settings-selector][issue408]") {
    auto theme = test_theme();
    coding_agent::tui::SettingsSelectorComponent selector(
        theme,
        test_keybindings(),
        coding_agent::tui::SettingsSelectorConfig{},
        coding_agent::tui::SettingsSelectorCallbacks{});
    const auto screen = render_screen(selector);
    CHECK(screen.find("Hide thinking") != std::string::npos);
    CHECK(screen.find("Theme") == std::string::npos);
}

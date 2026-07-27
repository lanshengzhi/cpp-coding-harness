#include "coding_agent/BoundedText.hpp"
#include "coding_agent/tui/Theme.hpp"

#include <cch/tui/Editor.hpp>
#include <cch/tui/Markdown.hpp>
#include <cch/tui/VirtualTerminal.hpp>

#include "../../../third_party/catch2/catch_test_macros.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

using namespace cch;

namespace {

[[nodiscard]] std::filesystem::path fixture_path(std::string_view name) {
    return std::filesystem::path(CCH_SOURCE_DIR) / "tests" / "fixtures" / "themes" / name;
}

[[nodiscard]] std::string read_fixture(std::string_view name) {
    std::ifstream input(fixture_path(name));
    std::ostringstream content;
    content << input.rdbuf();
    return content.str();
}

void replace_once(std::string& text, std::string_view old_text, std::string_view new_text) {
    const auto position = text.find(old_text);
    REQUIRE(position != std::string::npos);
    text.replace(position, old_text.size(), new_text);
}

[[nodiscard]] std::string valid_custom_theme() {
    auto json = read_fixture("dark.json");
    replace_once(json, "\"name\": \"dark\"", "\"name\": \"custom\"");
    replace_once(
        json,
        "\"customMsgBg\": \"#2d2838\"",
        "\"customMsgBg\": \"#2d2838\",\n\t\t\"fallbackA\": \"fallbackB\",\n\t\t\"fallbackB\": 17");
    replace_once(json, "\"accent\": \"accent\"", "\"accent\": \"#010203\"");
    replace_once(json, "\"text\": \"text\"", "\"text\": \"\"");
    replace_once(json, "\"selectedBg\": \"selectedBg\"", "\"selectedBg\": 255");
    replace_once(json, "\"toolPendingBg\": \"toolPendingBg\"", "\"toolPendingBg\": \"\"");
    replace_once(
        json,
        "\"thinkingXhigh\": \"#d183e8\",\n\t\t\"thinkingMax\": \"#ff5fff\"",
        "\"thinkingXhigh\": \"fallbackA\"");
    return json;
}

[[nodiscard]] std::string diagnostic_text(const util::Error& error) {
    return error.message + "\n" + error.detail;
}

void write_render_result(tui::VirtualTerminal& terminal, const tui::RenderResult& rendered) {
    for (std::size_t row = 0; row < rendered.lines.size(); ++row) {
        REQUIRE(terminal.set_cursor({.column = 0, .row = row}));
        REQUIRE(terminal.write(rendered.lines[row]));
    }
}

[[nodiscard]] std::string color_at_text(
    const tui::VirtualTerminal& terminal,
    std::string_view text,
    std::size_t offset = 0) {
    const auto screen = terminal.screen();
    for (std::size_t row = 0; row < screen.size(); ++row) {
        const auto column = screen[row].find(text);
        if (column != std::string::npos) {
            REQUIRE(column + offset < terminal.cells()[row].size());
            return terminal.cells()[row][column + offset].style.fg_color;
        }
    }
    REQUIRE(false);
    return {};
}

[[nodiscard]] double relative_luminance(const coding_agent::tui::RgbThemeColor& color) {
    const auto linear = [](std::uint8_t channel) {
        const auto value = static_cast<double>(channel) / 255.0;
        return value <= 0.03928 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * linear(color.red) + 0.7152 * linear(color.green) + 0.0722 * linear(color.blue);
}

[[nodiscard]] double contrast_ratio(
    const coding_agent::tui::ResolvedTheme& theme,
    coding_agent::tui::ThemeToken foreground,
    coding_agent::tui::ThemeToken background) {
    const auto* foreground_rgb = std::get_if<coding_agent::tui::RgbThemeColor>(
        &coding_agent::tui::color_for(theme, foreground));
    const auto* background_rgb = std::get_if<coding_agent::tui::RgbThemeColor>(
        &coding_agent::tui::color_for(theme, background));
    REQUIRE(foreground_rgb != nullptr);
    REQUIRE(background_rgb != nullptr);
    const auto foreground_luminance = relative_luminance(*foreground_rgb);
    const auto background_luminance = relative_luminance(*background_rgb);
    return (std::max(foreground_luminance, background_luminance) + 0.05) /
        (std::min(foreground_luminance, background_luminance) + 0.05);
}

} // namespace

TEST_CASE(
    "Built-in themes match baseline fixtures and resolve every semantic token",
    "[coding_agent][theme][issue55]") {
    const auto dark_fixture = coding_agent::tui::load_theme_file(fixture_path("dark.json"));
    const auto light_fixture = coding_agent::tui::load_theme_file(fixture_path("light.json"));

    REQUIRE(dark_fixture);
    REQUIRE(light_fixture);
    CHECK(*dark_fixture == coding_agent::tui::builtin_dark_theme());
    CHECK(*light_fixture == coding_agent::tui::builtin_light_theme());
    CHECK(coding_agent::tui::all_theme_tokens().size() == coding_agent::tui::kThemeTokenCount);
    CHECK(coding_agent::tui::kRequiredThemeTokenCount == 51);
    CHECK(coding_agent::tui::kThemeTokenCount == 52);
    for (const auto token : coding_agent::tui::all_theme_tokens()) {
        CHECK_FALSE(coding_agent::tui::theme_token_name(token).empty());
    }
}

TEST_CASE("Built-in themes keep content readable against semantic backgrounds", "[coding_agent][theme][issue55]") {
    constexpr std::array pairs{
        std::pair{coding_agent::tui::ThemeToken::UserMessageText, coding_agent::tui::ThemeToken::UserMessageBg},
        std::pair{coding_agent::tui::ThemeToken::CustomMessageText, coding_agent::tui::ThemeToken::CustomMessageBg},
        std::pair{coding_agent::tui::ThemeToken::ToolTitle, coding_agent::tui::ThemeToken::ToolPendingBg},
        std::pair{coding_agent::tui::ThemeToken::ToolTitle, coding_agent::tui::ThemeToken::ToolSuccessBg},
        std::pair{coding_agent::tui::ThemeToken::ToolTitle, coding_agent::tui::ThemeToken::ToolErrorBg},
        std::pair{coding_agent::tui::ThemeToken::Text, coding_agent::tui::ThemeToken::SelectedBg},
    };
    for (const auto& theme : {coding_agent::tui::builtin_dark_theme(), coding_agent::tui::builtin_light_theme()}) {
        for (const auto& [foreground, background] : pairs) {
            CHECK(contrast_ratio(theme, foreground, background) >= 4.5);
        }
    }
}

TEST_CASE(
    "Theme parsing accepts schema variables RGB xterm defaults references and thinking fallback",
    "[coding_agent][theme][issue55]") {
    const auto parsed = coding_agent::tui::parse_theme_json("custom fixture", valid_custom_theme());

    REQUIRE(parsed);
    CHECK(parsed->name == "custom");
    const auto* accent = std::get_if<coding_agent::tui::RgbThemeColor>(
        &coding_agent::tui::color_for(*parsed, coding_agent::tui::ThemeToken::Accent));
    REQUIRE(accent != nullptr);
    CHECK(accent->red == 1);
    CHECK(accent->green == 2);
    CHECK(accent->blue == 3);
    const auto* selected = std::get_if<coding_agent::tui::XtermThemeColor>(
        &coding_agent::tui::color_for(*parsed, coding_agent::tui::ThemeToken::SelectedBg));
    REQUIRE(selected != nullptr);
    CHECK(selected->index == 255);
    CHECK(std::holds_alternative<coding_agent::tui::TerminalDefaultThemeColor>(
        coding_agent::tui::color_for(*parsed, coding_agent::tui::ThemeToken::Text)));
    CHECK(std::holds_alternative<coding_agent::tui::TerminalDefaultThemeColor>(
        coding_agent::tui::color_for(*parsed, coding_agent::tui::ThemeToken::ToolPendingBg)));
    const auto* maximum = std::get_if<coding_agent::tui::XtermThemeColor>(
        &coding_agent::tui::color_for(*parsed, coding_agent::tui::ThemeToken::ThinkingMax));
    REQUIRE(maximum != nullptr);
    CHECK(maximum->index == 17);
}

TEST_CASE("Theme parsing reports invalid names and missing required tokens", "[coding_agent][theme][issue55]") {
    auto empty_name = valid_custom_theme();
    replace_once(empty_name, "\"name\": \"custom\"", "\"name\": \"\"");
    const auto empty = coding_agent::tui::parse_theme_json("empty name fixture", empty_name);
    REQUIRE_FALSE(empty);
    CHECK(diagnostic_text(empty.error()).find("at least one character") != std::string::npos);

    auto invalid_name = valid_custom_theme();
    replace_once(invalid_name, "\"name\": \"custom\"", "\"name\": \"bad/name\"");
    const auto named = coding_agent::tui::parse_theme_json("name fixture", invalid_name);
    REQUIRE_FALSE(named);
    CHECK(diagnostic_text(named.error()).find("cannot contain '/'") != std::string::npos);

    auto missing = valid_custom_theme();
    replace_once(missing, "\n\t\t\"accent\": \"#010203\",", "");
    const auto required = coding_agent::tui::parse_theme_json("missing fixture", missing);
    REQUIRE_FALSE(required);
    CHECK(diagnostic_text(required.error()).find("Missing required color tokens") != std::string::npos);
    CHECK(diagnostic_text(required.error()).find("accent") != std::string::npos);
}

TEST_CASE(
    "Theme parsing reports unresolved cyclic malformed and out-of-range colors",
    "[coding_agent][theme][issue55]") {
    auto unresolved_json = valid_custom_theme();
    replace_once(unresolved_json, "\"accent\": \"#010203\"", "\"accent\": \"missingRef\"");
    const auto unresolved = coding_agent::tui::parse_theme_json("unresolved fixture", unresolved_json);
    REQUIRE_FALSE(unresolved);
    CHECK(diagnostic_text(unresolved.error()).find("Variable reference not found: missingRef") != std::string::npos);

    auto cyclic_json = valid_custom_theme();
    replace_once(
        cyclic_json,
        "\"fallbackB\": 17",
        "\"fallbackB\": 17,\n\t\t\"cycleA\": \"cycleB\",\n\t\t\"cycleB\": \"cycleA\"");
    replace_once(cyclic_json, "\"accent\": \"#010203\"", "\"accent\": \"cycleA\"");
    const auto cyclic = coding_agent::tui::parse_theme_json("cycle fixture", cyclic_json);
    REQUIRE_FALSE(cyclic);
    CHECK(diagnostic_text(cyclic.error()).find("Circular variable reference") != std::string::npos);

    auto malformed_hex_json = valid_custom_theme();
    replace_once(malformed_hex_json, "\"accent\": \"#010203\"", "\"accent\": \"#xyz\"");
    const auto malformed_hex = coding_agent::tui::parse_theme_json("hex fixture", malformed_hex_json);
    REQUIRE_FALSE(malformed_hex);
    CHECK(diagnostic_text(malformed_hex.error()).find("#RRGGBB") != std::string::npos);

    auto range_json = valid_custom_theme();
    replace_once(range_json, "\"selectedBg\": 255", "\"selectedBg\": 256");
    const auto range = coding_agent::tui::parse_theme_json("range fixture", range_json);
    REQUIRE_FALSE(range);
    CHECK(diagnostic_text(range.error()).find("0..255") != std::string::npos);
}

TEST_CASE("Theme parsing rejects unknown schema members", "[coding_agent][theme][issue55]") {
    auto top_level_json = valid_custom_theme();
    replace_once(top_level_json, "\"name\": \"custom\",", "\"name\": \"custom\",\n\t\"version\": 1,");
    const auto top_level = coding_agent::tui::parse_theme_json("top-level fixture", top_level_json);
    REQUIRE_FALSE(top_level);
    CHECK(diagnostic_text(top_level.error()).find("unknown top-level field") != std::string::npos);

    auto color_json = valid_custom_theme();
    replace_once(color_json, "\"accent\": \"#010203\",", "\"accent\": \"#010203\",\n\t\t\"unknownColor\": 1,");
    const auto color = coding_agent::tui::parse_theme_json("color fixture", color_json);
    REQUIRE_FALSE(color);
    CHECK(diagnostic_text(color.error()).find("unknown color token") != std::string::npos);

    auto export_json = valid_custom_theme();
    replace_once(export_json, "\"pageBg\": \"#18181e\",", "\"pageBg\": \"#18181e\",\n\t\t\"unknownExport\": 1,");
    const auto exported = coding_agent::tui::parse_theme_json("export fixture", export_json);
    REQUIRE_FALSE(exported);
    CHECK(diagnostic_text(exported.error()).find("unknown export field") != std::string::npos);
}

TEST_CASE("Malformed theme JSON produces a bounded redacted diagnostic", "[coding_agent][theme][issue55]") {
    const auto malformed = coding_agent::tui::parse_theme_json(
        "malformed fixture",
        std::string{"{\"name\":\"sk-abcdefghijklmnopqrstuvwxyz123456\","} + std::string(10000, 'x'));

    REQUIRE_FALSE(malformed);
    CHECK(malformed.error().code == util::ErrorCode::JsonParse);
    CHECK(malformed.error().message.size() <= coding_agent::kMaxPresentationPayloadBytes);
    CHECK(malformed.error().detail.size() <= coding_agent::kMaxPresentationPayloadBytes);
    CHECK(diagnostic_text(malformed.error()).find("failed to parse theme") != std::string::npos);
    CHECK(diagnostic_text(malformed.error()).find("sk-abcdefghijklmnopqrstuvwxyz123456") == std::string::npos);
}

TEST_CASE("Theme styling selects safe built-ins and maps color capability", "[coding_agent][theme][issue55]") {
    const tui::TerminalCapabilities unknown;
    CHECK(coding_agent::tui::select_builtin_theme(unknown).name == "dark");
    const tui::TerminalCapabilities light{.appearance = tui::TerminalAppearance::Light};
    CHECK(coding_agent::tui::select_builtin_theme(light).name == "light");

    auto resolved = *coding_agent::tui::parse_theme_json("custom fixture", valid_custom_theme());
    coding_agent::tui::LiveTheme true_color(std::move(resolved), tui::TerminalColorCapability::TrueColor);
    CHECK(true_color.foreground(coding_agent::tui::ThemeToken::Accent, "x") ==
        "\x1b[38;2;1;2;3mx\x1b[39m");
    CHECK(true_color.background(coding_agent::tui::ThemeToken::SelectedBg, "x") ==
        "\x1b[48;5;255mx\x1b[49m");
    CHECK(true_color.foreground(coding_agent::tui::ThemeToken::Text, "x") ==
        "\x1b[39mx\x1b[39m");
    CHECK(true_color.background(coding_agent::tui::ThemeToken::ToolPendingBg, "x") ==
        "\x1b[49mx\x1b[49m");

    auto limited_json = valid_custom_theme();
    replace_once(limited_json, "\"accent\": \"#010203\"", "\"accent\": \"#808080\"");
    auto limited_resolved = *coding_agent::tui::parse_theme_json("limited fixture", limited_json);
    coding_agent::tui::LiveTheme limited(std::move(limited_resolved), tui::TerminalColorCapability::Xterm256);
    CHECK(limited.foreground(coding_agent::tui::ThemeToken::Accent, "x") ==
        "\x1b[38;5;244mx\x1b[39m");
}

TEST_CASE("Theme adapters resolve the current palette at callback time", "[coding_agent][theme][issue55]") {
    coding_agent::tui::LiveTheme live(
        coding_agent::tui::builtin_dark_theme(),
        tui::TerminalColorCapability::TrueColor);
    auto generic_status = live.foreground_hook(coding_agent::tui::ThemeToken::Accent);
    auto generic_tool = live.background_hook(coding_agent::tui::ThemeToken::ToolPendingBg);
    auto markdown_style = live.markdown_style();
    auto editor_theme = live.editor_theme();
    auto select_theme = live.select_list_theme();
    auto settings_theme = live.settings_list_theme();

    const auto dark_status = generic_status("status");
    const auto dark_tool = generic_tool("tool");
    const auto dark_heading = markdown_style.heading("heading");
    const auto dark_link_text = markdown_style.link_text("label");
    const auto dark_link_url = markdown_style.link_url(" (https://example.com)");
    const auto dark_selection = select_theme.selected_text("selected");
    const auto dark_setting = settings_theme.label("setting", true);
    tui::Editor editor;
    editor.set_theme(std::move(editor_theme));
    editor.set_text("editor");
    const auto dark_editor = editor.render(8);
    REQUIRE(dark_editor);

    live.replace(coding_agent::tui::builtin_light_theme(), tui::TerminalColorCapability::TrueColor);
    const auto light_status = generic_status("status");
    const auto light_tool = generic_tool("tool");
    const auto light_heading = markdown_style.heading("heading");
    const auto light_link_text = markdown_style.link_text("label");
    const auto light_link_url = markdown_style.link_url(" (https://example.com)");
    const auto light_selection = select_theme.selected_text("selected");
    const auto light_setting = settings_theme.label("setting", true);
    const auto light_editor = editor.render(8);
    REQUIRE(light_editor);

    CHECK(dark_status != light_status);
    CHECK(dark_tool != light_tool);
    CHECK(dark_heading != light_heading);
    CHECK(dark_link_text != light_link_text);
    CHECK(dark_link_url != light_link_url);
    CHECK(dark_selection != light_selection);
    CHECK(dark_setting != light_setting);
    CHECK(dark_editor->lines != light_editor->lines);

    tui::Markdown markdown(
        "plain\n\n# heading\n\n[label](https://example.com)",
        0,
        0,
        std::move(markdown_style));
    const auto light_markdown = markdown.render(40);
    REQUIRE(light_markdown);
    tui::VirtualTerminal light_terminal({.columns = 40, .rows = light_markdown->lines.size() + 1});
    REQUIRE(light_terminal.start([](std::string) {}, [](tui::TerminalDimensions) {}));
    write_render_result(light_terminal, *light_markdown);
    CHECK(color_at_text(light_terminal, "plain") == "38;2;31;35;40");
    CHECK(color_at_text(light_terminal, "heading") == "38;2;154;115;38");
    CHECK(color_at_text(light_terminal, "label") == "38;2;84;125;167");
    CHECK(color_at_text(light_terminal, "https://example.com") == "38;2;118;118;118");

    live.replace(coding_agent::tui::builtin_dark_theme(), tui::TerminalColorCapability::TrueColor);
    markdown.invalidate();
    const auto dark_markdown = markdown.render(40);
    REQUIRE(dark_markdown);
    tui::VirtualTerminal dark_terminal({.columns = 40, .rows = dark_markdown->lines.size() + 1});
    REQUIRE(dark_terminal.start([](std::string) {}, [](tui::TerminalDimensions) {}));
    write_render_result(dark_terminal, *dark_markdown);
    CHECK(light_markdown->lines != dark_markdown->lines);
    CHECK(color_at_text(dark_terminal, "plain") == "38;2;212;212;212");
    CHECK(color_at_text(dark_terminal, "heading") == "38;2;240;198;116");
    CHECK(color_at_text(dark_terminal, "label") == "38;2;129;162;190");
    CHECK(color_at_text(dark_terminal, "https://example.com") == "38;2;102;102;102");
}

#include "HtmlExport.hpp"

#include "agent/harness/WorkspaceFileSystem.hpp"
#include "coding_agent/tui/ThemeColorMath.hpp"
#include "support/Json.hpp"

#include <cch/tui/Base64.hpp>
#include <cch/tui/Utils.hpp>

#ifndef CCH_SOURCE_DIR
#define CCH_SOURCE_DIR ""
#endif

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace cch::coding_agent::tui {
namespace {

// pi `theme-tokens.ts` `THEME_TOKENS` slot classification for the C++ token
// subset (v1.3.0 descriptors; the C++ list predates the scrollbarTrack and
// searchMatch additions). pi's export emits foreground-slot tokens first,
// then background-slot tokens, each in theme-file order — and theme files
// follow the token schema order both sides share.
[[nodiscard]] bool is_background_slot(ThemeToken token) {
    switch (token) {
    case ThemeToken::SelectedBg:
    case ThemeToken::UserMessageBg:
    case ThemeToken::CustomMessageBg:
    case ThemeToken::ToolPendingBg:
    case ThemeToken::ToolSuccessBg:
    case ThemeToken::ToolErrorBg:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] support::Error export_error(std::string message) {
    return support::make_error(support::ErrorCode::Unknown, std::move(message));
}

// ── pi export-html/index.ts color math (verbatim port) ──────────────────────

struct CssRgb {
    int red;
    int green;
    int blue;
};

/// pi export-html `parseColor`: exactly `#RRGGBB` or `rgb(<digits>,
/// <digits>, <digits>)` — the only shapes `deriveExportColors` can receive
/// from `colorToHex`-produced theme colors.
[[nodiscard]] std::optional<CssRgb> parse_export_color(std::string_view value) {
    const auto decimal_channel = [](std::string_view text) -> std::optional<int> {
        if (text.empty() || text.size() > 3) return std::nullopt;
        int channel = 0;
        for (const char character : text) {
            if (character < '0' || character > '9') return std::nullopt;
            channel = channel * 10 + (character - '0');
        }
        return channel;
    };
    if (value.size() == 7 && value.front() == '#') {
        const auto hex_channel = [&](std::size_t offset) -> std::optional<int> {
            const auto digit = [](char character) -> std::optional<int> {
                if (character >= '0' && character <= '9') return character - '0';
                if (character >= 'a' && character <= 'f') return character - 'a' + 10;
                if (character >= 'A' && character <= 'F') return character - 'A' + 10;
                return std::nullopt;
            };
            const auto first = digit(value[1 + offset]);
            const auto second = digit(value[2 + offset]);
            if (!first || !second) return std::nullopt;
            return *first * 16 + *second;
        };
        const auto red = hex_channel(0);
        const auto green = hex_channel(2);
        const auto blue = hex_channel(4);
        if (!red || !green || !blue) return std::nullopt;
        return CssRgb{*red, *green, *blue};
    }
    constexpr std::string_view kPrefix = "rgb(";
    if (value.size() > kPrefix.size() && value.starts_with(kPrefix) && value.back() == ')') {
        const auto inner = value.substr(kPrefix.size(), value.size() - kPrefix.size() - 1);
        std::vector<std::string_view> parts;
        std::size_t start = 0;
        while (true) {
            const auto comma = inner.find(',', start);
            if (comma == std::string_view::npos) {
                parts.push_back(inner.substr(start));
                break;
            }
            parts.push_back(inner.substr(start, comma - start));
            start = comma + 1;
        }
        if (parts.size() != 3) return std::nullopt;
        const auto trim = [](std::string_view text) {
            while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
            while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
            return text;
        };
        const auto red = decimal_channel(trim(parts[0]));
        const auto green = decimal_channel(trim(parts[1]));
        const auto blue = decimal_channel(trim(parts[2]));
        if (!red || !green || !blue) return std::nullopt;
        return CssRgb{*red, *green, *blue};
    }
    return std::nullopt;
}

/// pi export-html `getLuminance`: WCAG relative luminance.
[[nodiscard]] double css_luminance(const CssRgb& color) {
    const auto linear = [](int channel) {
        const auto value = static_cast<double>(channel) / 255.0;
        return value <= 0.03928 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * linear(color.red) + 0.7152 * linear(color.green) + 0.0722 * linear(color.blue);
}

/// pi export-html `adjustBrightness`: `Math.round` matches `std::lround` for
/// the non-negative products here; the result keeps pi's `rgb(r, g, b)`
/// spelling.
[[nodiscard]] std::string adjust_brightness(const CssRgb& color, double factor) {
    const auto adjust = [&](int channel) {
        return std::min(255, std::max(0, static_cast<int>(std::lround(channel * factor))));
    };
    return std::format("rgb({}, {}, {})", adjust(color.red), adjust(color.green), adjust(color.blue));
}

struct DerivedExportColors {
    std::string page_bg;
    std::string card_bg;
    std::string info_bg;
};

/// pi export-html `deriveExportColors`.
[[nodiscard]] DerivedExportColors derive_export_colors(std::string_view base_color) {
    const auto parsed = parse_export_color(base_color);
    if (!parsed) {
        return {
                .page_bg = "rgb(24, 24, 30)",
                .card_bg = "rgb(30, 30, 36)",
                .info_bg = "rgb(60, 55, 40)",
        };
    }
    if (css_luminance(*parsed) > 0.5) {
        return {
                .page_bg = adjust_brightness(*parsed, 0.96),
                .card_bg = std::string{base_color},
                .info_bg = std::format(
                        "rgb({}, {}, {})",
                        std::min(255, parsed->red + 10),
                        std::min(255, parsed->green + 5),
                        std::max(0, parsed->blue - 20)),
        };
    }
    return {
            .page_bg = adjust_brightness(*parsed, 0.7),
            .card_bg = adjust_brightness(*parsed, 0.85),
            .info_bg = std::format(
                    "rgb({}, {}, {})",
                    std::min(255, parsed->red + 20),
                    std::min(255, parsed->green + 15),
                    parsed->blue),
    };
}

// ── pi theme.ts resolution pieces the export needs ──────────────────────────

[[nodiscard]] bool starts_with_color_function(std::string_view value, std::string_view function) {
    if (value.size() < function.size()) return false;
    for (std::size_t index = 0; index < function.size(); ++index) {
        auto character = value[index];
        if (character >= 'A' && character <= 'Z') character = static_cast<char>(character - 'A' + 'a');
        if (character != function[index]) return false;
    }
    return true;
}

/// pi theme.ts `detectAppearance`/`averageLightness`: palette colors 0-15
/// follow the user's terminal palette, so they say nothing about the theme;
/// appearance reads the averages of the remaining concrete slot colors.
[[nodiscard]] std::optional<TerminalAppearance> detect_theme_appearance(const ResolvedTheme& theme) {
    const auto average = [&](bool background) -> std::optional<double> {
        double sum = 0.0;
        std::size_t count = 0;
        for (const auto token : all_theme_tokens()) {
            if (is_background_slot(token) != background) continue;
            const auto& color = color_for(theme, token);
            std::optional<cch::tui::Color> concrete;
            if (const auto* rgb = std::get_if<RgbThemeColor>(&color)) {
                concrete = cch::tui::Color{
                        cch::tui::RgbColor{
                                .red = static_cast<double>(rgb->red),
                                .green = static_cast<double>(rgb->green),
                                .blue = static_cast<double>(rgb->blue),
                        },
                };
            } else if (const auto* indexed = std::get_if<XtermThemeColor>(&color); indexed && indexed->index >= 16) {
                concrete = cch::tui::Color{cch::tui::IndexedColor{indexed->index}};
            }
            if (!concrete) continue;
            const auto oklch = cch::tui::color_to_oklch(*concrete);
            if (!oklch) continue;
            sum += oklch->l;
            ++count;
        }
        if (count == 0) return std::nullopt;
        return sum / static_cast<double>(count);
    };
    const auto foreground = average(false);
    const auto background = average(true);
    if (foreground && background) {
        return *background < *foreground ? TerminalAppearance::Dark : TerminalAppearance::Light;
    }
    if (background) return *background < 0.5 ? TerminalAppearance::Dark : TerminalAppearance::Light;
    if (foreground) return *foreground > 0.5 ? TerminalAppearance::Dark : TerminalAppearance::Light;
    return std::nullopt;
}

/// One theme token resolved to the CSS value pi's `colorToHex` produces.
[[nodiscard]] support::Expected<std::string> theme_token_css(
        const ResolvedTheme& theme,
        ThemeToken token,
        const std::optional<cch::tui::TerminalColors>& terminal_colors,
        TerminalAppearance appearance) {
    const auto& color = color_for(theme, token);
    if (const auto* rgb = std::get_if<RgbThemeColor>(&color)) {
        return cch::tui::color_to_hex(cch::tui::Color{
                cch::tui::RgbColor{
                        .red = static_cast<double>(rgb->red),
                        .green = static_cast<double>(rgb->green),
                        .blue = static_cast<double>(rgb->blue),
                },
        });
    }
    if (const auto* indexed = std::get_if<XtermThemeColor>(&color)) {
        return cch::tui::color_to_hex(cch::tui::Color{cch::tui::IndexedColor{indexed->index}});
    }
    // pi `Theme.colors`: `""` (terminal-default) tokens take the reported
    // terminal color for the token's slot, else the appearance's guessed
    // pair (theme.ts `GUESSED_DEFAULT_COLORS`).
    const auto fallback = [&](bool foreground) {
        const bool dark = appearance == TerminalAppearance::Dark;
        if (foreground) {
            return dark ? cch::tui::RgbColor{.red = 229, .green = 229, .blue = 231}
                        : cch::tui::RgbColor{.red = 0, .green = 0, .blue = 0};
        }
        return dark ? cch::tui::RgbColor{.red = 0, .green = 0, .blue = 0}
                    : cch::tui::RgbColor{.red = 255, .green = 255, .blue = 255};
    };
    const bool background = is_background_slot(token);
    std::optional<cch::tui::RgbColor> reported;
    if (terminal_colors) reported = background ? terminal_colors->background : terminal_colors->foreground;
    return cch::tui::color_to_hex(cch::tui::Color{reported.value_or(fallback(!background))});
}

/// pi theme.ts `resolveVarRefs` over the retained raw `vars`: numbers, "",
/// hex, and oklch/okhsl functions pass through; any other string must chain
/// through `vars`, and unknown or circular names are errors (pi throws and
/// its `getThemeExportColors` catch discards the whole export section).
[[nodiscard]] support::Expected<std::string> resolve_export_reference(
        const ThemeExportValueVariant& value,
        const ResolvedTheme& theme,
        std::set<std::string, std::less<>>& visited) {
    const ThemeExportValueVariant* current = &value;
    while (true) {
        if (const auto* number = std::get_if<int>(current)) {
            if (*number < 0 || *number > 255) {
                return std::unexpected(export_error(
                        std::format("ANSI color index must be an integer from 0 to 255: {}", *number)));
            }
            const auto hex = cch::tui::color_to_hex(cch::tui::Color{cch::tui::IndexedColor{
                    static_cast<std::uint8_t>(*number),
            }});
            if (!hex) return std::unexpected(hex.error());
            return *hex;
        }
        const auto& text = std::get<std::string>(*current);
        if (text.empty() || text.starts_with('#') || starts_with_color_function(text, "oklch(") ||
                starts_with_color_function(text, "okhsl(")) {
            return text;
        }
        if (!visited.insert(text).second) {
            return std::unexpected(export_error("Circular variable reference detected: " + text));
        }
        const auto found = theme.vars.find(text);
        if (found == theme.vars.end()) {
            return std::unexpected(export_error("Variable reference not found: " + text));
        }
        current = &found->second;
    }
}

/// pi theme.ts `getThemeExportColors.resolve`: the resolved value becomes a
/// hex/index conversion, `""` reads as absent, and an okhsl function converts
/// to hex because CSS does not understand it (oklch passes through).
[[nodiscard]] support::Expected<std::optional<std::string>> resolve_export_color(
        const std::optional<ThemeExportValueVariant>& value,
        const ResolvedTheme& theme) {
    if (!value) return std::nullopt;
    std::set<std::string, std::less<>> visited;
    auto resolved = resolve_export_reference(*value, theme, visited);
    if (!resolved) return std::unexpected(resolved.error());
    if (resolved->empty()) return std::nullopt;
    if (starts_with_color_function(*resolved, "okhsl(")) {
        const auto color = cch::tui::parse_color(*resolved);
        if (!color) return std::unexpected(color.error());
        const auto hex = cch::tui::color_to_hex(*color);
        if (!hex) return std::unexpected(hex.error());
        return *hex;
    }
    return *resolved;
}

// ── pi export-html/index.ts template substitution ───────────────────────────

/// pi builds the export with JavaScript `String.prototype.replace(search,
/// replacement)`: a string search replaces the first occurrence only, and the
/// replacement string expands `$$`/`$&`/`$``/`$'` (`$$` → `$`, `$&` → the
/// matched placeholder, `$``/`$'` → the text around the match). The vendored
/// bundles contain `$$` (template.js cost line) and `$&` (highlight.js
/// operator regex), so a plain literal replacement would not reproduce pi's
/// output — this helper implements the exact semantics.
[[nodiscard]] std::string replace_first_js(
        const std::string& text,
        const std::string& needle,
        const std::string& replacement) {
    const auto position = text.find(needle);
    if (position == std::string::npos) return text;
    std::string result;
    result.reserve(text.size() + replacement.size());
    result.append(text, 0, position);
    for (std::size_t index = 0; index < replacement.size(); ++index) {
        if (replacement[index] == '$' && index + 1 < replacement.size()) {
            const char next = replacement[index + 1];
            if (next == '$') {
                result.push_back('$');
                ++index;
                continue;
            }
            if (next == '&') {
                result.append(needle);
                ++index;
                continue;
            }
            if (next == '`') {
                result.append(text, 0, position);
                ++index;
                continue;
            }
            if (next == '\'') {
                result.append(text, position + needle.size(), std::string::npos);
                ++index;
                continue;
            }
        }
        result.push_back(replacement[index]);
    }
    result.append(text, position + needle.size(), std::string::npos);
    return result;
}

[[nodiscard]] support::Expected<std::string> read_text_asset(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return std::unexpected(export_error("Export template asset not readable: " + path.string()));
    return std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

/// The vendored pi template directory: `share/pike/export-html` of the
/// installed prefix (the executable lives in its `bin/`), falling back to
/// the committed third_party copy in a source build — the same resolution
/// shape as the codemode guest wasm.
[[nodiscard]] std::filesystem::path export_html_template_dir() {
    std::error_code error;
    const auto executable = std::filesystem::read_symlink("/proc/self/exe", error);
    if (!error) {
        auto installed = executable.parent_path().parent_path() / "share" / "pike" / "export-html";
        if (std::filesystem::exists(installed, error)) return installed;
    }
    return std::filesystem::path{CCH_SOURCE_DIR} / "third_party" / "pi-export-html";
}

[[nodiscard]] support::Expected<std::string> json_string(std::string_view value) {
    const auto encoded = support::write_json(support::JsonValue{std::string{value}});
    if (!encoded) return std::unexpected(encoded.error());
    return *encoded;
}

} // namespace

support::Expected<HtmlExportTheme> make_html_export_theme(
        const ResolvedTheme& theme,
        const std::optional<cch::tui::TerminalColors>& terminal_colors,
        TerminalAppearance terminal_appearance) {
    // pi `Theme.appearance`: the theme's own detected appearance wins; the
    // terminal's scheme only stands in when the theme has no detectable
    // colors of its own.
    const auto appearance = detect_theme_appearance(theme).value_or(terminal_appearance);

    HtmlExportTheme result;
    // pi `getResolvedThemeColors` iterates pi's theme colors, whose order is
    // foreground-slot tokens then background-slot tokens (the theme file's
    // key order, partitioned by slot in pi's `splitThemeColors`).
    for (const bool background : {false, true}) {
        for (const auto token : all_theme_tokens()) {
            if (is_background_slot(token) != background) continue;
            auto css = theme_token_css(theme, token, terminal_colors, appearance);
            if (!css) return std::unexpected(css.error());
            result.css_vars.emplace_back(std::string{theme_token_name(token)}, std::move(*css));
        }
    }

    // pi `getThemeExportColors`: any resolution failure discards the whole
    // explicit section (pi's single try/catch), leaving the derived fallback.
    const auto page = resolve_export_color(theme.export_colors.pageBg, theme);
    const auto card = resolve_export_color(theme.export_colors.cardBg, theme);
    const auto info = resolve_export_color(theme.export_colors.infoBg, theme);
    const bool explicit_section_usable = page.has_value() && card.has_value() && info.has_value();

    // pi `generateThemeVars`: the base is the resolved userMessageBg hex
    // (`colors.userMessageBg || "#343541"` — the latter is JS-undefined
    // defense that cannot trigger for a concrete C++ token).
    const auto user_bg = theme_token_css(theme, ThemeToken::UserMessageBg, terminal_colors, appearance);
    if (!user_bg) return std::unexpected(user_bg.error());
    const auto derived = derive_export_colors(user_bg->empty() ? "#343541" : *user_bg);
    result.page_bg = (explicit_section_usable && page->has_value()) ? **page : derived.page_bg;
    result.card_bg = (explicit_section_usable && card->has_value()) ? **card : derived.card_bg;
    result.info_bg = (explicit_section_usable && info->has_value()) ? **info : derived.info_bg;
    return result;
}

support::Expected<std::string> build_html_session_data_json(
        cch::harness::session::SessionStore& store,
        const HtmlExportExtras& extras) {
    // pi `SessionManager.getEntries()`: every entry of the session in file
    // order, each as its pi wire line. The harness tree reading excludes the
    // header line and unknown entry kinds; every remaining kind round-trips
    // through the package-private serializer as its pi wire line.
    auto snapshot = store.export_wire_snapshot();
    if (!snapshot) return std::unexpected(snapshot.error());

    std::string document = R"({"header":)";
    document += snapshot->header;
    document += R"(,"entries":[)";
    bool first = true;
    for (const auto& line : snapshot->entries) {
        if (!first) document.push_back(',');
        first = false;
        document += line;
    }
    document += R"(],"leafId":)";
    if (snapshot->leaf_id.empty()) {
        document += "null";
    } else {
        auto encoded = json_string(snapshot->leaf_id);
        if (!encoded) return std::unexpected(encoded.error());
        document += *encoded;
    }
    document.push_back('}');

    // The optional pi `AgentState` members are appended in pi's key order.
    // Building the document by string assembly (rather than the key-sorted
    // `support::JsonValue` object model) keeps pi's member order and the
    // wire lines' field order byte-stable inside the base64 payload.
    std::string with_extras = std::move(document);
    if (extras.system_prompt.has_value() && !extras.system_prompt->empty()) {
        auto encoded = json_string(*extras.system_prompt);
        if (!encoded) return std::unexpected(encoded.error());
        with_extras.insert(with_extras.size() - 1, R"(,"systemPrompt":)" + *encoded);
    }
    if (!extras.tools.empty()) {
        std::string tools = R"(,"tools":[)";
        bool first_tool = true;
        for (const auto& tool : extras.tools) {
            if (!first_tool) tools.push_back(',');
            first_tool = false;
            auto name = json_string(tool.name);
            if (!name) return std::unexpected(name.error());
            auto description = json_string(tool.description);
            if (!description) return std::unexpected(description.error());
            auto parameters = support::write_json(tool.parameters);
            if (!parameters) return std::unexpected(parameters.error());
            tools += R"({"name":)" + *name + R"(,"description":)" + *description + R"(,"parameters":)" +
                    *parameters + "}";
        }
        tools.push_back(']');
        with_extras.insert(with_extras.size() - 1, std::move(tools));
    }
    return with_extras;
}

support::Expected<std::string> generate_session_html(
        std::string_view session_data_json,
        const HtmlExportTheme& theme) {
    const auto template_dir = export_html_template_dir();
    auto template_html = read_text_asset(template_dir / "template.html");
    if (!template_html) return std::unexpected(template_html.error());
    auto template_css = read_text_asset(template_dir / "template.css");
    if (!template_css) return std::unexpected(template_css.error());
    auto template_js = read_text_asset(template_dir / "template.js");
    if (!template_js) return std::unexpected(template_js.error());
    auto marked_js = read_text_asset(template_dir / "vendor" / "marked.min.js");
    if (!marked_js) return std::unexpected(marked_js.error());
    auto hljs_js = read_text_asset(template_dir / "vendor" / "highlight.min.js");
    if (!hljs_js) return std::unexpected(hljs_js.error());

    // pi `generateThemeVars`: one `--token: value;` line per theme color
    // (already in pi's emission order) plus the three export backgrounds,
    // joined so every line after the first lands on the template.css
    // six-space indent.
    std::vector<std::string> lines;
    lines.reserve(theme.css_vars.size() + 3);
    for (const auto& [name, value] : theme.css_vars) {
        lines.push_back(std::format("--{}: {};", name, value));
    }
    lines.push_back(std::format("--exportPageBg: {};", theme.page_bg));
    lines.push_back(std::format("--exportCardBg: {};", theme.card_bg));
    lines.push_back(std::format("--exportInfoBg: {};", theme.info_bg));
    std::string theme_vars;
    for (std::size_t index = 0; index < lines.size(); ++index) {
        if (index > 0) theme_vars += "\n      ";
        theme_vars += lines[index];
    }

    // pi `generateHtml`: CSS first, then the document, both through the
    // JavaScript replacement semantics documented on `replace_first_js`.
    auto css = replace_first_js(*template_css, "{{THEME_VARS}}", theme_vars);
    css = replace_first_js(css, "{{BODY_BG}}", theme.page_bg);
    css = replace_first_js(css, "{{CONTAINER_BG}}", theme.card_bg);
    css = replace_first_js(css, "{{INFO_BG}}", theme.info_bg);

    const auto base64_payload = cch::tui::base64_encode(session_data_json);
    auto html = replace_first_js(*template_html, "{{CSS}}", css);
    html = replace_first_js(html, "{{JS}}", *template_js);
    html = replace_first_js(html, "{{SESSION_DATA}}", base64_payload);
    html = replace_first_js(html, "{{MARKED_JS}}", *marked_js);
    html = replace_first_js(html, "{{HIGHLIGHT_JS}}", *hljs_js);
    return html;
}

support::Expected<std::string> export_session_to_html(
        cch::harness::session::SessionStore& store,
        std::string_view output_path,
        const std::filesystem::path& session_file,
        const HtmlExportTheme& theme,
        const HtmlExportExtras& extras) {
    auto document = build_html_session_data_json(store, extras);
    if (!document) return std::unexpected(document.error());
    auto html = generate_session_html(*document, theme);
    if (!html) return std::unexpected(html.error());

    std::string destination{output_path};
    if (destination.empty()) {
        // pi `exportSessionToHtml`: `${APP_NAME}-session-${basename}.html`,
        // relative to the working directory; the C++ binary's identity is
        // "pike" (pi's APP_NAME is config-derived, default "pi").
        destination = "pike-session-" + session_file.stem().string() + ".html";
    }
    cch::harness::WorkspaceFileSystem filesystem;
    const auto written = filesystem.write_file(destination, *html, true, {});
    if (!written) return std::unexpected(written.error());
    return destination;
}

} // namespace cch::coding_agent::tui

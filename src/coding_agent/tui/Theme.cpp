#include "Theme.hpp"

#include "BuiltinThemes.hpp"
#include "coding_agent/BoundedText.hpp"
#include "support/Json.hpp"

#include <cch/support/Error.hpp>
#include <cch/tui/Utils.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <format>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace cch::coding_agent::tui {
namespace {

using RawColorValue = std::variant<std::string, int>;
using RawColorMap = std::map<std::string, RawColorValue, std::less<>>;

constexpr std::array<ThemeToken, kThemeTokenCount> kAllThemeTokens{
#define CCH_THEME_TOKEN(enum_name, wire_name, required) ThemeToken::enum_name,
#include "ThemeTokens.inc"
#undef CCH_THEME_TOKEN
};

constexpr std::array<std::string_view, kThemeTokenCount> kThemeTokenNames{
#define CCH_THEME_TOKEN(enum_name, wire_name, required) wire_name,
#include "ThemeTokens.inc"
#undef CCH_THEME_TOKEN
};

constexpr std::array<bool, kThemeTokenCount> kThemeTokenRequired{
#define CCH_THEME_TOKEN(enum_name, wire_name, required) required != 0,
#include "ThemeTokens.inc"
#undef CCH_THEME_TOKEN
};

static_assert(kAllThemeTokens.size() == kThemeTokenNames.size());
static_assert(static_cast<std::size_t>(ThemeToken::BashMode) + 1 == kThemeTokenCount);

[[nodiscard]] support::Error theme_error(
    support::ErrorCode code,
    std::string message,
    std::string detail = {}) {
    return support::make_error(
        code,
        bounded_redacted_presentation(std::move(message)),
        bounded_redacted_presentation(std::move(detail)));
}

/// Bounds a pi-verbatim validation message without re-running secret
/// redaction over the fixed wording: the redactor's secret-key heuristic
/// would mangle pi's "Missing required color tokens:" lines. Every
/// user-controlled fragment is bounded and redacted before composition.
[[nodiscard]] support::Error verbatim_validation_error(support::ErrorCode code, std::string message) {
    return support::make_error(code, bounded_presentation(std::move(message)));
}

[[nodiscard]] std::string safe_label(std::string_view label) {
    return bounded_redacted_presentation(std::string(label));
}

[[nodiscard]] std::optional<ThemeToken> token_for_name(std::string_view name) {
    for (std::size_t index = 0; index < kThemeTokenNames.size(); ++index) {
        if (kThemeTokenNames[index] == name) return kAllThemeTokens[index];
    }
    return std::nullopt;
}

/// One typebox-verbatim schema error line (`  - <path>: <message>`), shaped
/// exactly like pi's `parseThemeJson` "Other errors" entries (theme.ts).
struct SchemaErrorLine {
    std::string path;
    std::string message;
};

/// Composes pi's verbatim `parseThemeJson` error message: the missing-color
/// block first, then the "Other errors" block, both byte-for-byte (theme.ts).
[[nodiscard]] std::string invalid_theme_message(
    std::string_view label,
    const std::vector<std::string>& missing_colors,
    const std::vector<SchemaErrorLine>& other_errors) {
    // pi sorts the collected missing names before listing them.
    auto sorted_missing = missing_colors;
    std::sort(sorted_missing.begin(), sorted_missing.end());
    std::string message = std::format("Invalid theme \"{}\":\n", safe_label(label));
    if (!sorted_missing.empty()) {
        message += "\nMissing required color tokens:\n";
        for (std::size_t index = 0; index < sorted_missing.size(); ++index) {
            if (index > 0) message += "\n";
            message += "  - " + sorted_missing[index];
        }
        message += "\n\nPlease add these colors to your theme's \"colors\" object.\n";
        message += "See the built-in themes (dark.json, light.json) for reference values.";
    }
    if (!other_errors.empty()) {
        message += "\n\nOther errors:\n";
        for (std::size_t index = 0; index < other_errors.size(); ++index) {
            if (index > 0) message += "\n";
            message += "  - " + other_errors[index].path + ": " + other_errors[index].message;
        }
    }
    return message;
}

/// Appends pi's typebox-verbatim error lines when `value` does not satisfy
/// the ColorValueSchema union (any string, or an integer in 0..255); returns
/// the raw value otherwise. The per-case line set mirrors Ajv's output for
/// the union `[Type.String(), Type.Integer({minimum: 0, maximum: 255})]`.
[[nodiscard]] std::optional<RawColorValue> check_color_value(
    std::vector<SchemaErrorLine>& errors,
    std::string path,
    const support::JsonValue& value) {
    if (const auto* text = value.get_if<std::string>()) return *text;
    if (const auto* number = value.get_if<double>()) {
        const auto integral = std::floor(*number) == *number;
        if (integral && *number >= 0 && *number <= 255) {
            return static_cast<int>(*number);
        }
        errors.push_back({path, "must be string"});
        if (!integral) errors.push_back({path, "must be integer"});
        if (*number < 0) errors.push_back({path, "must be >= 0"});
        if (*number > 255) errors.push_back({path, "must be <= 255"});
        errors.push_back({path, "must match a schema in anyOf"});
        return std::nullopt;
    }
    errors.push_back({path, "must be string"});
    errors.push_back({path, "must be integer"});
    errors.push_back({path, "must match a schema in anyOf"});
    return std::nullopt;
}

[[nodiscard]] support::Expected<RgbThemeColor> parse_rgb(std::string_view value) {
    auto color = cch::tui::parse_color(value);
    if (!color || !std::holds_alternative<cch::tui::RgbColor>(*color)) {
        return std::unexpected(verbatim_validation_error(
            support::ErrorCode::Validation,
            "Invalid hex color: " + bounded_redacted_presentation(std::string(value))));
    }
    const auto& rgb = std::get<cch::tui::RgbColor>(*color);
    const auto checked = cch::tui::rgb_color(rgb.red, rgb.green, rgb.blue);
    if (!checked) return std::unexpected(checked.error());
    return RgbThemeColor{
            .red = static_cast<std::uint8_t>(checked->red),
            .green = static_cast<std::uint8_t>(checked->green),
            .blue = static_cast<std::uint8_t>(checked->blue),
    };
}

[[nodiscard]] support::Expected<ResolvedThemeColor> resolve_color(
    const RawColorValue& value,
    const RawColorMap& variables) {
    const RawColorValue* current = &value;
    std::set<std::string, std::less<>> visited;
    while (true) {
        if (const auto* index = std::get_if<int>(current)) {
            return XtermThemeColor{.index = static_cast<std::uint8_t>(*index)};
        }
        const auto& text = std::get<std::string>(*current);
        if (text.empty()) return TerminalDefaultThemeColor{};
        if (text.starts_with('#')) {
            auto rgb = parse_rgb(text);
            if (!rgb) return std::unexpected(rgb.error());
            return *rgb;
        }
        if (visited.contains(text)) {
            return std::unexpected(verbatim_validation_error(
                support::ErrorCode::Validation,
                "Circular variable reference detected: " +
                    bounded_redacted_presentation(text)));
        }
        const auto found = variables.find(text);
        if (found == variables.end()) {
            return std::unexpected(verbatim_validation_error(
                support::ErrorCode::Validation,
                "Variable reference not found: " + bounded_redacted_presentation(text)));
        }
        visited.insert(text);
        current = &found->second;
    }
}

[[nodiscard]] ResolvedTheme required_builtin(std::string_view label, std::string_view json) {
    auto result = parse_theme_json(label, json);
    if (!result) std::terminate();
    return std::move(*result);
}

[[nodiscard]] std::string attribute_style(cch::tui::TextStyle style, std::string text) {
    auto styled = cch::tui::style_text(text, style, cch::tui::TerminalColorMode::TrueColor);
    if (!styled) std::terminate();
    return std::move(*styled);
}

} // namespace

std::span<const ThemeToken> all_theme_tokens() {
    return kAllThemeTokens;
}

std::string_view theme_token_name(ThemeToken token) {
    return kThemeTokenNames[static_cast<std::size_t>(token)];
}

const ResolvedThemeColor& color_for(const ResolvedTheme& theme, ThemeToken token) {
    return theme.colors[static_cast<std::size_t>(token)];
}

namespace {

[[nodiscard]] support::Expected<ResolvedTheme> resolve_theme_value(
    std::string_view label,
    const support::JsonValue& parsed) {
    std::vector<SchemaErrorLine> other_errors;
    std::vector<std::string> missing_colors;

    const auto* root = parsed.get_if<support::JsonValue::object_t>();
    if (root == nullptr) {
        return std::unexpected(verbatim_validation_error(
            support::ErrorCode::Validation,
            invalid_theme_message(label, missing_colors, {{ "/", "must be object" }})));
    }

    if (const auto schema = root->find("$schema"); schema != root->end()) {
        if (schema->second.get_if<std::string>() == nullptr) {
            other_errors.push_back({"/$schema", "must be string"});
        }
    }
    std::string name;
    const auto name_value = root->find("name");
    if (name_value == root->end()) {
        other_errors.push_back({"/", "must have required properties name"});
    } else if (const auto* name_string = name_value->second.get_if<std::string>()) {
        name = *name_string;
    } else {
        other_errors.push_back({"/name", "must be string"});
    }

    RawColorMap variables;
    if (const auto vars_value = root->find("vars"); vars_value != root->end()) {
        const auto* vars_object = vars_value->second.get_if<support::JsonValue::object_t>();
        if (vars_object == nullptr) {
            other_errors.push_back({"/vars", "must be object"});
        } else {
            for (const auto& [variable_name, value] : *vars_object) {
                auto raw = check_color_value(
                    other_errors,
                    bounded_redacted_presentation("/vars/" + variable_name),
                    value);
                if (!raw) continue;
                variables.emplace(variable_name, std::move(*raw));
            }
        }
    }

    RawColorMap raw_colors;
    ThemeExportColors export_colors;
    const support::JsonValue::object_t* colors_object = nullptr;
    const auto colors_value = root->find("colors");
    if (colors_value == root->end()) {
        other_errors.push_back({"/", "must have required properties colors"});
    } else if (colors_object = colors_value->second.get_if<support::JsonValue::object_t>();
               colors_object == nullptr) {
        other_errors.push_back({"/colors", "must be object"});
    } else {
        for (const auto& [color_name, value] : *colors_object) {
            if (!token_for_name(color_name)) {
                // pi's runtime schema accepts unknown color tokens; only
                // string/integer values are retained and resolved (pi crashes
                // on other value types). Unknown tokens never reach the
                // resolved theme.
                if (const auto* text = value.get_if<std::string>()) {
                    raw_colors.emplace(color_name, *text);
                } else if (const auto* number = value.get_if<double>()) {
                    if (std::floor(*number) == *number && *number >= 0 && *number <= 255) {
                        raw_colors.emplace(color_name, static_cast<int>(*number));
                    }
                }
                continue;
            }
            auto raw = check_color_value(
                other_errors,
                bounded_redacted_presentation("/colors/" + color_name),
                value);
            if (!raw) continue;
            raw_colors.emplace(color_name, std::move(*raw));
        }
    }

    if (const auto export_value = root->find("export"); export_value != root->end()) {
        const auto* export_object = export_value->second.get_if<support::JsonValue::object_t>();
        if (export_object == nullptr) {
            other_errors.push_back({"/export", "must be object"});
        } else {
            for (const auto& [export_name, value] : *export_object) {
                // Unknown export fields are accepted and ignored; pi's runtime
                // schema validates only the three known keys.
                if (export_name != "pageBg" && export_name != "cardBg" && export_name != "infoBg") {
                    continue;
                }
                auto raw = check_color_value(
                    other_errors,
                    bounded_redacted_presentation("/export/" + export_name),
                    value);
                if (!raw) continue;
                if (export_name == "pageBg") {
                    export_colors.pageBg = std::move(*raw);
                } else if (export_name == "cardBg") {
                    export_colors.cardBg = std::move(*raw);
                } else {
                    export_colors.infoBg = std::move(*raw);
                }
            }
        }
    }

    // Missing-token presence is checked against the colors object itself, so
    // a present-but-invalid value is reported as a value error only, like pi.
    if (colors_object != nullptr) {
        for (std::size_t index = 0; index < kAllThemeTokens.size(); ++index) {
            if (!kThemeTokenRequired[index]) continue;
            const auto name_view = kThemeTokenNames[index];
            if (colors_object->find(std::string(name_view)) == colors_object->end()) {
                missing_colors.emplace_back(name_view);
            }
        }
    }
    if (!missing_colors.empty() || !other_errors.empty()) {
        return std::unexpected(verbatim_validation_error(
            support::ErrorCode::Validation,
            invalid_theme_message(label, missing_colors, other_errors)));
    }

    // pi's withThemeColorFallbacks: optional tokens fall back to the raw
    // value of their companion token (both companions are required, so the
    // schema failure above would have returned already).
    if (!raw_colors.contains("thinkingMax")) {
        raw_colors.emplace("thinkingMax", raw_colors.at("thinkingXhigh"));
    }
    if (!raw_colors.contains("scrollbarThumb")) {
        raw_colors.emplace("scrollbarThumb", raw_colors.at("selectedBg"));
    }

    // pi's assertThemeNameIsValid runs after schema validation and rejects
    // the slash reserved for automatic light/dark theme settings.
    if (name.find('/') != std::string::npos) {
        return std::unexpected(verbatim_validation_error(
            support::ErrorCode::Validation,
            std::format(
                "Invalid theme name \"{}\": theme names cannot contain \"/\" because it is reserved for automatic light/dark theme settings.",
                bounded_redacted_presentation(name))));
    }

    // pi's createTheme resolves every colors entry (including unknown tokens
    // and fallbacks) before splitting fg/bg; any failure aborts the load.
    std::map<std::string, ResolvedThemeColor, std::less<>> resolved_colors;
    for (const auto& [color_name, raw] : raw_colors) {
        auto resolved = resolve_color(raw, variables);
        if (!resolved) return std::unexpected(resolved.error());
        resolved_colors.emplace(color_name, std::move(*resolved));
    }

    ResolvedTheme theme{.name = std::move(name), .export_colors = std::move(export_colors)};
    for (const auto token : kAllThemeTokens) {
        theme.colors[static_cast<std::size_t>(token)] =
            std::move(resolved_colors.at(std::string(theme_token_name(token))));
    }
    return theme;
}

} // namespace

support::Expected<ResolvedTheme> parse_theme_json(std::string_view label, std::string_view json) {
    auto parsed = support::read_json(json);
    if (!parsed) {
        const auto& parse_error = parsed.error();
        const auto& detail = parse_error.detail.empty() ? parse_error.message : parse_error.detail;
        return std::unexpected(verbatim_validation_error(
            support::ErrorCode::JsonParse,
            "Failed to parse theme " + safe_label(label) + ": " +
                bounded_redacted_presentation(detail)));
    }
    return resolve_theme_value(label, *parsed);
}

support::Expected<ResolvedTheme> load_theme_file(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input.is_open()) {
        return std::unexpected(theme_error(
            support::ErrorCode::Validation,
            "failed to load theme file",
            "could not open explicit theme path '" + path.string() + "'"));
    }
    std::ostringstream content;
    content << input.rdbuf();
    if (!input.good() && !input.eof()) {
        return std::unexpected(theme_error(
            support::ErrorCode::Validation,
            "failed to load theme file",
            "could not read explicit theme path '" + path.string() + "'"));
    }
    return parse_theme_json(path.string(), content.str());
}

ResolvedTheme builtin_dark_theme() {
    return required_builtin("built-in dark", detail::kBuiltinDarkThemeJson);
}

ResolvedTheme builtin_light_theme() {
    return required_builtin("built-in light", detail::kBuiltinLightThemeJson);
}

ResolvedTheme select_builtin_theme(const cch::tui::TerminalCapabilities& capabilities) {
    return capabilities.appearance == cch::tui::TerminalAppearance::Light
        ? builtin_light_theme()
        : builtin_dark_theme();
}

struct LiveTheme::Impl {
    Impl(ResolvedTheme configured_theme, cch::tui::TerminalColorCapability configured_capability)
        : theme(std::move(configured_theme)), capability(configured_capability) {}

    std::mutex mutex;
    ResolvedTheme theme;
    cch::tui::TerminalColorCapability capability{cch::tui::TerminalColorCapability::Xterm256};
};

std::string LiveTheme::apply_style(
    const std::shared_ptr<Impl>& impl,
    ThemeToken token,
    std::string text,
    bool background) {
    std::lock_guard lock(impl->mutex);
    const auto& resolved = color_for(impl->theme, token);
    std::optional<cch::tui::Color> foreground;
    std::optional<cch::tui::Color> background_color;
    if (const auto* rgb = std::get_if<RgbThemeColor>(&resolved)) {
        const cch::tui::RgbColor value{
                .red = static_cast<double>(rgb->red),
                .green = static_cast<double>(rgb->green),
                .blue = static_cast<double>(rgb->blue),
        };
        (background ? background_color : foreground) = value;
    } else if (const auto* indexed = std::get_if<XtermThemeColor>(&resolved)) {
        const cch::tui::IndexedColor value{.index = indexed->index};
        (background ? background_color : foreground) = value;
    }
    cch::tui::TextStyle style{
            .foreground = std::move(foreground),
            .background = std::move(background_color),
    };
    if (std::holds_alternative<TerminalDefaultThemeColor>(resolved)) {
        return cch::tui::style_text_with_ansi(text,
                background ? std::nullopt : std::optional<std::string_view>{"\x1b[39m"},
                background ? std::optional<std::string_view>{"\x1b[49m"} : std::nullopt,
                style);
    }
    auto styled = cch::tui::style_text(text,
            style,
            impl->capability == cch::tui::TerminalColorCapability::TrueColor ? cch::tui::TerminalColorMode::TrueColor
                                                                             : cch::tui::TerminalColorMode::Xterm256);
    if (!styled) std::terminate();
    return std::move(*styled);
}

LiveTheme::LiveTheme(
    ResolvedTheme theme,
    cch::tui::TerminalColorCapability capability)
    : impl_(std::make_shared<Impl>(std::move(theme), capability)) {}

LiveTheme::LiveTheme(LiveTheme&&) noexcept = default;
LiveTheme& LiveTheme::operator=(LiveTheme&&) noexcept = default;
LiveTheme::~LiveTheme() = default;

void LiveTheme::replace(
    ResolvedTheme theme,
    cch::tui::TerminalColorCapability capability) {
    std::lock_guard lock(impl_->mutex);
    impl_->theme = std::move(theme);
    impl_->capability = capability;
}

std::string LiveTheme::foreground(ThemeToken token, std::string text) const {
    return apply_style(impl_, token, std::move(text), false);
}

std::string LiveTheme::background(ThemeToken token, std::string text) const {
    return apply_style(impl_, token, std::move(text), true);
}

cch::tui::TextStyleHook LiveTheme::foreground_hook(ThemeToken token) const {
    const auto state = impl_;
    return [state, token](std::string text) {
        return apply_style(state, token, std::move(text), false);
    };
}

cch::tui::BackgroundHook LiveTheme::background_hook(ThemeToken token) const {
    const auto state = impl_;
    return [state, token](std::string text) {
        return apply_style(state, token, std::move(text), true);
    };
}

cch::tui::MarkdownStyleConfig LiveTheme::markdown_style() const {
    cch::tui::MarkdownStyleConfig style;
    // pi leaves the base text unstyled: assistant messages pass
    // `defaultTextStyle: undefined` (assistant-message.ts), and the callers
    // that style body text override `.text` (user-message.ts
    // fg("userMessageText"), custom-message.ts fg("customMessageText"),
    // thinking runs use thinkingText italic).
    style.heading = foreground_hook(ThemeToken::MdHeading);
    style.emphasis = [](std::string text) {
        return attribute_style(
                cch::tui::TextStyle{.foreground = std::nullopt, .background = std::nullopt, .italic = true},
                std::move(text));
    };
    style.strong = [](std::string text) {
        return attribute_style(
                cch::tui::TextStyle{.foreground = std::nullopt, .background = std::nullopt, .bold = true},
                std::move(text));
    };
    style.underline = [](std::string text) {
        return attribute_style(
                cch::tui::TextStyle{.foreground = std::nullopt, .background = std::nullopt, .underline = true},
                std::move(text));
    };
    style.strikethrough = [](std::string text) {
        return attribute_style(
                cch::tui::TextStyle{.foreground = std::nullopt, .background = std::nullopt, .strikethrough = true},
                std::move(text));
    };
    style.inline_code = foreground_hook(ThemeToken::MdCode);
    style.code_block = foreground_hook(ThemeToken::MdCodeBlock);
    style.code_block_border = foreground_hook(ThemeToken::MdCodeBlockBorder);
    style.list_marker = foreground_hook(ThemeToken::MdListBullet);
    style.quote = foreground_hook(ThemeToken::MdQuote);
    style.quote_border = foreground_hook(ThemeToken::MdQuoteBorder);
    style.horizontal_rule = foreground_hook(ThemeToken::MdHr);
    const auto state = impl_;
    style.link_text = [state](std::string text) {
        return apply_style(state, ThemeToken::MdLink, std::move(text), false);
    };
    style.link_url = foreground_hook(ThemeToken::MdLinkUrl);
    return style;
}

cch::tui::SelectListTheme LiveTheme::select_list_theme() const {
    return {
        .selected_text = foreground_hook(ThemeToken::Accent),
        .description = foreground_hook(ThemeToken::Muted),
        .scroll_info = foreground_hook(ThemeToken::Muted),
        .no_match = foreground_hook(ThemeToken::Muted),
    };
}

cch::tui::SettingsListTheme LiveTheme::settings_list_theme() const {
    const auto state = impl_;
    return {
            .label =
                    [state](std::string text, bool selected) {
                        return selected ? apply_style(state, ThemeToken::Accent, std::move(text), false) : text;
                    },
            .value =
                    [state](std::string text, bool selected) {
                        return apply_style(
                                state, selected ? ThemeToken::Accent : ThemeToken::Muted, std::move(text), false);
                    },
            .description = foreground_hook(ThemeToken::Dim),
            // pi `getSettingsListTheme`: `cursor: theme.fg("accent", "→ ")`. The
            // glyph is accent-styled, so it shares a styled run with the selected
            // label instead of rendering as an unstyled prefix.
            .cursor = apply_style(state, ThemeToken::Accent, "→ ", false),
            .hint = foreground_hook(ThemeToken::Dim),
    };
}

} // namespace cch::coding_agent::tui

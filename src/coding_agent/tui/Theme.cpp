#include "Theme.hpp"

#include "BuiltinThemes.hpp"
#include "coding_agent/BoundedText.hpp"
#include "util/Json.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <format>
#include <fstream>
#include <limits>
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

[[nodiscard]] util::Error theme_error(
    util::ErrorCode code,
    std::string message,
    std::string detail = {}) {
    return util::make_error(
        code,
        bounded_redacted_presentation(std::move(message)),
        bounded_redacted_presentation(std::move(detail)));
}

/// Bounds a pi-verbatim validation message without re-running secret
/// redaction over the fixed wording: the redactor's secret-key heuristic
/// would mangle pi's "Missing required color tokens:" lines. Every
/// user-controlled fragment is bounded and redacted before composition.
[[nodiscard]] util::Error verbatim_validation_error(util::ErrorCode code, std::string message) {
    return util::make_error(code, bounded_presentation(std::move(message)));
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
    const util::JsonValue& value) {
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

[[nodiscard]] std::optional<int> hex_digit(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return 10 + value - 'a';
    if (value >= 'A' && value <= 'F') return 10 + value - 'A';
    return std::nullopt;
}

[[nodiscard]] util::Expected<RgbThemeColor> parse_rgb(std::string_view value) {
    // Mirrors pi's hexToRgb (theme.ts): exactly six characters after '#', with
    // each channel pair parsed like parseInt(pair, 16) — the first character
    // must be a hex digit, and a non-hex second character ends the parse.
    // pi's sign/whitespace prefixes would yield channel values outside the
    // uint8_t color model and are rejected here as C++ hardening.
    if (value.size() != 7) {
        return std::unexpected(verbatim_validation_error(
            util::ErrorCode::Validation,
            "Invalid hex color: " + bounded_redacted_presentation(std::string(value))));
    }
    const auto channel = [&](std::size_t offset) -> std::optional<std::uint8_t> {
        const auto high = hex_digit(value[offset]);
        if (!high) return std::nullopt;
        if (const auto low = hex_digit(value[offset + 1])) {
            return static_cast<std::uint8_t>(*high * 16 + *low);
        }
        return static_cast<std::uint8_t>(*high);
    };
    const auto red = channel(1);
    const auto green = channel(3);
    const auto blue = channel(5);
    if (!red || !green || !blue) {
        return std::unexpected(verbatim_validation_error(
            util::ErrorCode::Validation,
            "Invalid hex color: " + bounded_redacted_presentation(std::string(value))));
    }
    return RgbThemeColor{.red = *red, .green = *green, .blue = *blue};
}

[[nodiscard]] util::Expected<ResolvedThemeColor> resolve_color(
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
                util::ErrorCode::Validation,
                "Circular variable reference detected: " +
                    bounded_redacted_presentation(text)));
        }
        const auto found = variables.find(text);
        if (found == variables.end()) {
            return std::unexpected(verbatim_validation_error(
                util::ErrorCode::Validation,
                "Variable reference not found: " + bounded_redacted_presentation(text)));
        }
        visited.insert(text);
        current = &found->second;
    }
}

[[nodiscard]] int closest_index(int value, std::span<const int> candidates) {
    int best_index = 0;
    int best_distance = std::numeric_limits<int>::max();
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        const auto distance = std::abs(value - candidates[index]);
        if (distance < best_distance) {
            best_distance = distance;
            best_index = static_cast<int>(index);
        }
    }
    return best_index;
}

[[nodiscard]] double color_distance(
    int red,
    int green,
    int blue,
    int candidate_red,
    int candidate_green,
    int candidate_blue) {
    const auto red_delta = red - candidate_red;
    const auto green_delta = green - candidate_green;
    const auto blue_delta = blue - candidate_blue;
    return red_delta * red_delta * 0.299 +
        green_delta * green_delta * 0.587 +
        blue_delta * blue_delta * 0.114;
}

[[nodiscard]] int rgb_to_xterm(const RgbThemeColor& color) {
    constexpr std::array<int, 6> cube{0, 95, 135, 175, 215, 255};
    constexpr std::array<int, 24> gray{
        8, 18, 28, 38, 48, 58, 68, 78, 88, 98, 108, 118,
        128, 138, 148, 158, 168, 178, 188, 198, 208, 218, 228, 238,
    };
    const auto red = static_cast<int>(color.red);
    const auto green = static_cast<int>(color.green);
    const auto blue = static_cast<int>(color.blue);
    const auto red_index = closest_index(red, cube);
    const auto green_index = closest_index(green, cube);
    const auto blue_index = closest_index(blue, cube);
    const auto cube_index = 16 + 36 * red_index + 6 * green_index + blue_index;
    const auto cube_distance = color_distance(
        red,
        green,
        blue,
        cube[static_cast<std::size_t>(red_index)],
        cube[static_cast<std::size_t>(green_index)],
        cube[static_cast<std::size_t>(blue_index)]);
    const auto neutral = static_cast<int>(std::round(0.299 * red + 0.587 * green + 0.114 * blue));
    const auto gray_offset = closest_index(neutral, gray);
    const auto gray_value = gray[static_cast<std::size_t>(gray_offset)];
    const auto gray_distance = color_distance(red, green, blue, gray_value, gray_value, gray_value);
    const auto spread = std::max({red, green, blue}) - std::min({red, green, blue});
    return spread < 10 && gray_distance < cube_distance ? 232 + gray_offset : cube_index;
}

[[nodiscard]] std::string ansi_prefix(
    const ResolvedThemeColor& color,
    cch::tui::TerminalColorCapability capability,
    bool background) {
    const auto channel = background ? 48 : 38;
    const auto reset = background ? 49 : 39;
    if (std::holds_alternative<TerminalDefaultThemeColor>(color)) {
        return std::format("\x1b[{}m", reset);
    }
    if (const auto* xterm = std::get_if<XtermThemeColor>(&color)) {
        return std::format("\x1b[{};5;{}m", channel, xterm->index);
    }
    const auto& rgb = std::get<RgbThemeColor>(color);
    if (capability == cch::tui::TerminalColorCapability::TrueColor) {
        return std::format("\x1b[{};2;{};{};{}m", channel, rgb.red, rgb.green, rgb.blue);
    }
    return std::format("\x1b[{};5;{}m", channel, rgb_to_xterm(rgb));
}

[[nodiscard]] ResolvedTheme required_builtin(std::string_view label, std::string_view json) {
    auto result = parse_theme_json(label, json);
    if (!result) std::terminate();
    return std::move(*result);
}

[[nodiscard]] std::string attribute_style(int begin, int end, std::string text) {
    return std::format("\x1b[{}m{}\x1b[{}m", begin, text, end);
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

[[nodiscard]] util::Expected<ResolvedTheme> resolve_theme_value(
    std::string_view label,
    const util::JsonValue& parsed) {
    std::vector<SchemaErrorLine> other_errors;
    std::vector<std::string> missing_colors;

    const auto* root = parsed.get_if<util::JsonValue::object_t>();
    if (root == nullptr) {
        return std::unexpected(verbatim_validation_error(
            util::ErrorCode::Validation,
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
        const auto* vars_object = vars_value->second.get_if<util::JsonValue::object_t>();
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
    const util::JsonValue::object_t* colors_object = nullptr;
    const auto colors_value = root->find("colors");
    if (colors_value == root->end()) {
        other_errors.push_back({"/", "must have required properties colors"});
    } else if (colors_object = colors_value->second.get_if<util::JsonValue::object_t>();
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
        const auto* export_object = export_value->second.get_if<util::JsonValue::object_t>();
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
            util::ErrorCode::Validation,
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
            util::ErrorCode::Validation,
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

util::Expected<ResolvedTheme> parse_theme_json(std::string_view label, std::string_view json) {
    auto parsed = util::read_json<util::JsonValue>(json);
    if (!parsed) {
        const auto& parse_error = parsed.error();
        const auto& detail = parse_error.detail.empty() ? parse_error.message : parse_error.detail;
        return std::unexpected(verbatim_validation_error(
            util::ErrorCode::JsonParse,
            "Failed to parse theme " + safe_label(label) + ": " +
                bounded_redacted_presentation(detail)));
    }
    return resolve_theme_value(label, *parsed);
}

util::Expected<ResolvedTheme> load_theme_file(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input.is_open()) {
        return std::unexpected(theme_error(
            util::ErrorCode::Validation,
            "failed to load theme file",
            "could not open explicit theme path '" + path.string() + "'"));
    }
    std::ostringstream content;
    content << input.rdbuf();
    if (!input.good() && !input.eof()) {
        return std::unexpected(theme_error(
            util::ErrorCode::Validation,
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
    const auto prefix = ansi_prefix(color_for(impl->theme, token), impl->capability, background);
    return prefix + text + (background ? "\x1b[49m" : "\x1b[39m");
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
    style.text = foreground_hook(ThemeToken::Text);
    style.heading = foreground_hook(ThemeToken::MdHeading);
    style.emphasis = [](std::string text) { return attribute_style(3, 23, std::move(text)); };
    style.strong = [](std::string text) { return attribute_style(1, 22, std::move(text)); };
    style.strikethrough = [](std::string text) { return attribute_style(9, 29, std::move(text)); };
    style.inline_code = foreground_hook(ThemeToken::MdCode);
    style.code_block = foreground_hook(ThemeToken::MdCodeBlock);
    style.code_block_border = foreground_hook(ThemeToken::MdCodeBlockBorder);
    style.list_marker = foreground_hook(ThemeToken::MdListBullet);
    style.quote = foreground_hook(ThemeToken::MdQuote);
    style.quote_border = foreground_hook(ThemeToken::MdQuoteBorder);
    style.horizontal_rule = foreground_hook(ThemeToken::MdHr);
    const auto state = impl_;
    style.link_text = [state](std::string text) {
        return attribute_style(
            4,
            24,
            apply_style(state, ThemeToken::MdLink, std::move(text), false));
    };
    style.link_url = foreground_hook(ThemeToken::MdLinkUrl);
    return style;
}

cch::tui::EditorTheme LiveTheme::editor_theme() const {
    return {.text = foreground_hook(ThemeToken::Text)};
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
        .label = [state](std::string text, bool selected) {
            return selected ? apply_style(state, ThemeToken::Accent, std::move(text), false) : text;
        },
        .value = [state](std::string text, bool selected) {
            return apply_style(
                state,
                selected ? ThemeToken::Accent : ThemeToken::Muted,
                std::move(text),
                false);
        },
        .description = foreground_hook(ThemeToken::Dim),
        .cursor = "→ ",
        .hint = foreground_hook(ThemeToken::Dim),
    };
}

} // namespace cch::coding_agent::tui

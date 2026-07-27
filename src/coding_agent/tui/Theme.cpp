#include "Theme.hpp"

#include "BuiltinThemes.hpp"
#include "coding_agent/BoundedText.hpp"
#include "util/Json.hpp"

#include <algorithm>
#include <array>
#include <cctype>
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

[[nodiscard]] std::string safe_label(std::string_view label) {
    return bounded_redacted_presentation(std::string(label));
}

[[nodiscard]] std::optional<ThemeToken> token_for_name(std::string_view name) {
    for (std::size_t index = 0; index < kThemeTokenNames.size(); ++index) {
        if (kThemeTokenNames[index] == name) return kAllThemeTokens[index];
    }
    return std::nullopt;
}

[[nodiscard]] util::Expected<const util::JsonValue::object_t*> require_object(
    const util::JsonValue& value,
    std::string path) {
    const auto* object = value.get_if<util::JsonValue::object_t>();
    if (object != nullptr) return object;
    return std::unexpected(theme_error(
        util::ErrorCode::Validation,
        "invalid theme schema",
        std::move(path) + " must be a JSON object"));
}

[[nodiscard]] util::Expected<RawColorValue> parse_raw_color(
    const util::JsonValue& value,
    std::string path) {
    if (const auto* text = value.get_if<std::string>()) {
        if (text->starts_with('#')) {
            const auto valid_hex = text->size() == 7 &&
                std::all_of(text->begin() + 1, text->end(), [](unsigned char digit) {
                    return std::isxdigit(digit) != 0;
                });
            if (!valid_hex) {
                return std::unexpected(theme_error(
                    util::ErrorCode::Validation,
                    "invalid theme color",
                    std::move(path) + " must use exact #RRGGBB syntax"));
            }
        }
        return *text;
    }
    if (const auto* number = value.get_if<double>()) {
        if (std::floor(*number) != *number || *number < 0 || *number > 255) {
            return std::unexpected(theme_error(
                util::ErrorCode::Validation,
                "invalid theme color",
                std::move(path) + " xterm palette index must be an integer in 0..255"));
        }
        return static_cast<int>(*number);
    }
    return std::unexpected(theme_error(
        util::ErrorCode::Validation,
        "invalid theme color",
        std::move(path) + " must be #RRGGBB, a variable reference, an empty string, or an integer in 0..255"));
}

[[nodiscard]] util::ExpectedVoid reject_unknown_fields(
    const util::JsonValue::object_t& object,
    std::span<const std::string_view> allowed,
    std::string_view description) {
    for (const auto& [name, value] : object) {
        (void)value;
        if (std::find(allowed.begin(), allowed.end(), name) == allowed.end()) {
            return std::unexpected(theme_error(
                util::ErrorCode::Validation,
                "invalid theme schema",
                std::format("unknown {} '{}'", description, name)));
        }
    }
    return {};
}

[[nodiscard]] int hex_digit(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return 10 + value - 'a';
    return 10 + value - 'A';
}

[[nodiscard]] RgbThemeColor parse_rgb(std::string_view value) {
    const auto channel = [&](std::size_t offset) {
        return static_cast<std::uint8_t>(hex_digit(value[offset]) * 16 + hex_digit(value[offset + 1]));
    };
    return {.red = channel(1), .green = channel(3), .blue = channel(5)};
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
        if (text.starts_with('#')) return parse_rgb(text);
        if (visited.contains(text)) {
            return std::unexpected(theme_error(
                util::ErrorCode::Validation,
                "invalid theme variable reference",
                "Circular variable reference detected: " + text));
        }
        const auto found = variables.find(text);
        if (found == variables.end()) {
            return std::unexpected(theme_error(
                util::ErrorCode::Validation,
                "invalid theme variable reference",
                "Variable reference not found: " + text));
        }
        visited.insert(text);
        current = &found->second;
    }
}

[[nodiscard]] std::string join_names(std::vector<std::string> names) {
    std::sort(names.begin(), names.end());
    std::string result;
    for (const auto& name : names) result += "\n  - " + name;
    return result;
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

[[nodiscard]] util::Expected<ResolvedTheme> resolve_theme_value(const util::JsonValue& parsed) {
    auto root_result = require_object(parsed, "/");
    if (!root_result) return std::unexpected(root_result.error());
    const auto& root = **root_result;
    constexpr std::array<std::string_view, 5> top_level_fields{
        "$schema", "name", "vars", "colors", "export"};
    if (auto checked = reject_unknown_fields(root, top_level_fields, "top-level field"); !checked) {
        return std::unexpected(checked.error());
    }

    if (const auto schema = root.find("$schema");
        schema != root.end() && schema->second.get_if<std::string>() == nullptr) {
        return std::unexpected(theme_error(
            util::ErrorCode::Validation,
            "invalid theme schema",
            "/$schema must be a string"));
    }
    const auto name_value = root.find("name");
    if (name_value == root.end() || name_value->second.get_if<std::string>() == nullptr) {
        return std::unexpected(theme_error(
            util::ErrorCode::Validation,
            "invalid theme schema",
            "/name is required and must be a string"));
    }
    const auto& name = *name_value->second.get_if<std::string>();
    if (name.empty()) {
        return std::unexpected(theme_error(
            util::ErrorCode::Validation,
            "invalid theme name",
            "theme name must contain at least one character"));
    }
    if (name.find('/') != std::string::npos) {
        return std::unexpected(theme_error(
            util::ErrorCode::Validation,
            "invalid theme name",
            "theme name '" + name + "' cannot contain '/'"));
    }

    RawColorMap variables;
    if (const auto vars_value = root.find("vars"); vars_value != root.end()) {
        auto vars_result = require_object(vars_value->second, "/vars");
        if (!vars_result) return std::unexpected(vars_result.error());
        for (const auto& [variable_name, value] : **vars_result) {
            auto raw = parse_raw_color(value, "/vars/" + variable_name);
            if (!raw) return std::unexpected(raw.error());
            variables.emplace(variable_name, std::move(*raw));
        }
    }

    const auto colors_value = root.find("colors");
    if (colors_value == root.end()) {
        return std::unexpected(theme_error(
            util::ErrorCode::Validation,
            "invalid theme schema",
            "/colors is required and must be an object"));
    }
    auto colors_result = require_object(colors_value->second, "/colors");
    if (!colors_result) return std::unexpected(colors_result.error());
    const auto& colors_object = **colors_result;
    RawColorMap raw_colors;
    for (const auto& [color_name, value] : colors_object) {
        if (!token_for_name(color_name)) {
            return std::unexpected(theme_error(
                util::ErrorCode::Validation,
                "invalid theme schema",
                "unknown color token '" + color_name + "'"));
        }
        auto raw = parse_raw_color(value, "/colors/" + color_name);
        if (!raw) return std::unexpected(raw.error());
        raw_colors.emplace(color_name, std::move(*raw));
    }

    std::vector<std::string> missing;
    for (std::size_t index = 0; index < kAllThemeTokens.size(); ++index) {
        if (!kThemeTokenRequired[index]) continue;
        const auto name_view = kThemeTokenNames[index];
        if (!raw_colors.contains(name_view)) missing.emplace_back(name_view);
    }
    if (!missing.empty()) {
        return std::unexpected(theme_error(
            util::ErrorCode::Validation,
            "invalid theme schema",
            "Missing required color tokens:" + join_names(std::move(missing)) +
                "\nAdd these colors to the theme's colors object; see the built-in dark and light themes."));
    }
    if (!raw_colors.contains("thinkingMax")) {
        raw_colors.emplace("thinkingMax", raw_colors.at("thinkingXhigh"));
    }

    if (const auto export_value = root.find("export"); export_value != root.end()) {
        auto export_result = require_object(export_value->second, "/export");
        if (!export_result) return std::unexpected(export_result.error());
        constexpr std::array<std::string_view, 3> export_fields{"pageBg", "cardBg", "infoBg"};
        if (auto checked = reject_unknown_fields(**export_result, export_fields, "export field"); !checked) {
            return std::unexpected(checked.error());
        }
        for (const auto& [export_name, value] : **export_result) {
            auto raw = parse_raw_color(value, "/export/" + export_name);
            if (!raw) return std::unexpected(raw.error());
            auto resolved = resolve_color(*raw, variables);
            if (!resolved) return std::unexpected(resolved.error());
        }
    }

    ResolvedTheme theme{.name = name};
    for (const auto token : kAllThemeTokens) {
        auto resolved = resolve_color(
            raw_colors.at(std::string(theme_token_name(token))),
            variables);
        if (!resolved) return std::unexpected(resolved.error());
        theme.colors[static_cast<std::size_t>(token)] = std::move(*resolved);
    }
    return theme;
}

} // namespace

util::Expected<ResolvedTheme> parse_theme_json(std::string_view label, std::string_view json) {
    auto parsed = util::read_json<util::JsonValue>(json);
    if (!parsed) {
        return std::unexpected(theme_error(
            util::ErrorCode::JsonParse,
            "failed to parse theme " + safe_label(label),
            parsed.error().detail));
    }
    auto resolved = resolve_theme_value(*parsed);
    if (!resolved) {
        auto error = resolved.error();
        error.message = bounded_redacted_presentation(
            std::format("invalid theme '{}': {}", safe_label(label), error.message));
        return std::unexpected(std::move(error));
    }
    return resolved;
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

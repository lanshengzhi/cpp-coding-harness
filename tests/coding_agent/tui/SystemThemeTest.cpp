// The generated `system` theme (#1032): the generator port (families,
// per-token contrast curves, OKHSL anchoring, relaxation bisection, the
// pending grayscale state and the no-report indexed tier) compared
// per-token against the frozen pi-v1.3.0 audit-1032 capture, plus the
// pi-verbatim faint (SGR 2) rendering of the `dim` token set.

#include "coding_agent/tui/SystemTheme.hpp"
#include "coding_agent/tui/ThemeColorMath.hpp"
#include "support/PiTuiEvidence.hpp"

#include <cch/tui/TerminalColors.hpp>
#include <cch/tui/Utils.hpp>

#include <cch/support/Error.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace cch;

namespace {

[[nodiscard]] const support::JsonValue& scenario_of(const support::JsonValue& artifact, std::string_view name) {
    for (const auto& scenario : artifact.at("scenarios").get_array()) {
        if (scenario.at("name").get_string() == name) return scenario;
    }
    REQUIRE(false);
    return artifact;
}

[[nodiscard]] std::map<std::string, coding_agent::tui::ThemeToken, std::less<>> tokens_by_wire_name() {
    std::map<std::string, coding_agent::tui::ThemeToken, std::less<>> by_name;
    for (const auto token : coding_agent::tui::all_theme_tokens()) {
        by_name.emplace(coding_agent::tui::theme_token_name(token), token);
    }
    return by_name;
}

[[nodiscard]] tui::RgbColor rgb_of(std::string_view hex) {
    REQUIRE((hex.size() == 7 && hex.front() == '#'));
    const auto channel = [&](std::size_t offset) {
        unsigned int value = 0;
        for (const char digit : hex.substr(offset, 2)) {
            value *= 16;
            if (digit >= '0' && digit <= '9') {
                value += static_cast<unsigned int>(digit - '0');
            } else {
                value += static_cast<unsigned int>(digit - 'a' + 10);
            }
        }
        return static_cast<double>(value);
    };
    return tui::RgbColor{.red = channel(1), .green = channel(3), .blue = channel(5)};
}

[[nodiscard]] coding_agent::tui::SystemThemeInput input_of(const support::JsonValue& scenario) {
    const auto& inputs = scenario.at("inputs");
    const auto& fields = inputs.get_object();
    const auto string_field = [&fields](std::string_view key) -> const std::string* {
        const auto found = fields.find(std::string{key});
        if (found == fields.end()) return nullptr;
        return found->second.get_if<std::string>();
    };
    coding_agent::tui::SystemThemeInput input;
    input.saturation = inputs.at("saturation").get_number();
    if (const auto* foreground = string_field("foreground")) {
        input.foreground = rgb_of(*foreground);
    }
    if (const auto* background = string_field("background")) {
        input.background = rgb_of(*background);
    }
    if (const auto* hint = string_field("appearanceHint")) {
        input.appearance_hint = *hint == "dark" ? coding_agent::tui::TerminalAppearance::Dark
                                                : coding_agent::tui::TerminalAppearance::Light;
    }
    if (const auto palette_field = fields.find("palette"); palette_field != fields.end()) {
        if (palette_field->second.get_if<support::JsonValue::array_t>() != nullptr) {
            std::vector<tui::RgbColor> palette;
            for (const auto& entry : palette_field->second.get_array()) {
                palette.push_back(rgb_of(entry.get_string()));
            }
            input.palette = std::move(palette);
        }
    }
    return input;
}

void check_matches(const coding_agent::tui::SystemThemeColors& generated, const support::JsonValue& scenario) {
    using namespace coding_agent::tui;
    const auto tokens = tokens_by_wire_name();
    const auto& expected = scenario.at("expected");
    const auto& colors = expected.at("colors").get_object();
    REQUIRE(colors.size() == kThemeTokenCount);
    for (const auto& [name, value] : colors) {
        const auto token = tokens.at(name);
        const auto& actual = generated.colors[static_cast<std::size_t>(token)];
        if (const auto* hex = value.get_if<std::string>()) {
            if (hex->empty()) {
                CHECK(std::holds_alternative<TerminalDefaultThemeColor>(actual));
            } else {
                const RgbThemeColor expected_color{.red = static_cast<std::uint8_t>(rgb_of(*hex).red),
                        .green = static_cast<std::uint8_t>(rgb_of(*hex).green),
                        .blue = static_cast<std::uint8_t>(rgb_of(*hex).blue)};
                CHECK(actual == ResolvedThemeColor{expected_color});
            }
        } else {
            const auto index = static_cast<std::uint8_t>(value.get_number());
            CHECK(actual == ResolvedThemeColor{XtermThemeColor{.index = index}});
        }
    }
    if (expected.at("appearance").get_if<support::JsonValue::null_t>() != nullptr) {
        CHECK_FALSE(generated.appearance.has_value());
    } else {
        REQUIRE(generated.appearance.has_value());
        CHECK(*generated.appearance == (expected.at("appearance").get_string() == "dark" ? TerminalAppearance::Dark
                                                                                         : TerminalAppearance::Light));
    }
    const auto& dim = expected.at("dim").get_array();
    REQUIRE(generated.dim.size() == dim.size());
    for (std::size_t index = 0; index < dim.size(); ++index) {
        CHECK(generated.dim[index] == tokens.at(dim[index].get_string()));
    }
}

void check_text_contrast(const coding_agent::tui::SystemThemeColors& generated, const support::JsonValue& scenario) {
    using namespace coding_agent::tui;
    const auto tokens = tokens_by_wire_name();
    const auto& inputs = scenario.at("inputs");
    const auto effective = [&](ThemeToken token) -> std::optional<tui::RgbColor> {
        const auto& color = generated.colors[static_cast<std::size_t>(token)];
        if (std::holds_alternative<TerminalDefaultThemeColor>(color)) {
            if (const auto* foreground = inputs.at("foreground").get_if<std::string>()) return rgb_of(*foreground);
            return std::nullopt;
        }
        const auto* rgb = std::get_if<RgbThemeColor>(&color);
        if (rgb == nullptr) return std::nullopt;
        return tui::RgbColor{.red = static_cast<double>(rgb->red),
                .green = static_cast<double>(rgb->green),
                .blue = static_cast<double>(rgb->blue)};
    };
    const auto surface_color = [&](std::string_view surface) -> std::optional<tui::RgbColor> {
        if (surface == "background") {
            if (const auto* background = inputs.at("background").get_if<std::string>()) return rgb_of(*background);
            return std::nullopt;
        }
        return effective(tokens.at(std::string{surface}));
    };
    const auto& contrast = scenario.at("expected").at("textContrast");
    for (const auto& [token_name, observation] : contrast.get_object()) {
        const auto color = effective(tokens.at(token_name));
        REQUIRE(color.has_value());
        double minimum = std::numeric_limits<double>::infinity();
        for (const auto& [surface, ratio] : observation.at("surfaces").get_object()) {
            const auto on = surface_color(surface);
            REQUIRE(on.has_value());
            const auto computed = wcag_contrast(*color, *on);
            CHECK(computed == ratio.get_number());
            minimum = std::min(minimum, computed);
        }
        CHECK(observation.at("minimum").get_number() == minimum);
        CHECK(minimum >= 4.5);
    }
}

} // namespace

TEST_CASE("generated system theme matches the frozen capture in every reported-color tier",
        "[coding_agent][theme][system][issue1032][spec]") {
    const auto artifact = tests::read_pi_v130_audit_candidate("audit-1032", "system-theme-capture.json");
    REQUIRE(artifact);
    for (const auto& scenario : artifact->at("scenarios").get_array()) {
        const auto name = scenario.at("name").get_string();
        CAPTURE(name);
        const auto generated = coding_agent::tui::generate_system_theme_colors(input_of(scenario));
        check_matches(generated, scenario);
        if (scenario.at("expected").get_object().contains("textContrast")) {
            check_text_contrast(generated, scenario);
        }
    }
}

TEST_CASE("generated system theme keeps the frozen contract edge cases",
        "[coding_agent][theme][system][issue1032][spec]") {
    using namespace coding_agent::tui;
    // A partial palette is ignored: the families' own hues generate instead
    // (pi `input.palette?.length === 16`).
    SystemThemeInput partial;
    partial.foreground = tui::RgbColor{.red = 0xe5, .green = 0xe5, .blue = 0xe7};
    partial.background = tui::RgbColor{.red = 0, .green = 0, .blue = 0};
    partial.palette = std::vector<tui::RgbColor>(15, *partial.foreground);
    const auto from_partial = generate_system_theme_colors(partial);
    partial.palette = std::nullopt;
    const auto from_none = generate_system_theme_colors(partial);
    CHECK(from_partial.colors == from_none.colors);

    // The saturation clamp: out-of-range saturation values clamp to
    // [0, 1] like pi's `clamp(input.saturation ?? 1, 0, 1)`.
    SystemThemeInput base;
    base.background = tui::RgbColor{.red = 0, .green = 0, .blue = 0};
    base.saturation = 2.0;
    const auto clamped_high = generate_system_theme_colors(base);
    base.saturation = 1.0;
    const auto full = generate_system_theme_colors(base);
    CHECK(clamped_high.colors == full.colors);
    base.saturation = -1.0;
    const auto clamped_low = generate_system_theme_colors(base);
    base.saturation = 0.0;
    const auto grayscale = generate_system_theme_colors(base);
    CHECK(clamped_low.colors == grayscale.colors);

    // `system_theme_from_colors` names the theme and carries appearance/dim.
    const auto indexed = generate_system_theme_colors(SystemThemeInput{});
    const auto theme = system_theme_from_colors(indexed);
    CHECK(theme.name == "system");
    CHECK(theme.appearance == indexed.appearance);
    CHECK(theme.dim == indexed.dim);
}

TEST_CASE("faint system tokens render with pi's verbatim SGR 2 byte sequence",
        "[coding_agent][theme][system][issue1032][spec]") {
    using namespace coding_agent::tui;
    // The no-report tier: neutral tokens below body text are faint and the
    // terminal renders their "" default itself.
    const auto indexed = system_theme_from_colors(generate_system_theme_colors(SystemThemeInput{}));
    LiveTheme live(indexed, tui::TerminalColorCapability::TrueColor);
    CHECK(live.foreground(ThemeToken::Muted, "x") == "\x1b[39m\x1b[2mx\x1b[22;39m");
    CHECK(live.foreground(ThemeToken::ToolTitle, "x") == "\x1b[39mx\x1b[39m");
    CHECK(live.background(ThemeToken::Muted, "x") == "\x1b[49mx\x1b[49m");

    // The settled tier takes ANSI slots and does not mark tokens faint.
    SystemThemeInput input;
    input.saturation = 1.0;
    const auto settled = system_theme_from_colors(generate_system_theme_colors(input));
    LiveTheme settled_live(settled, tui::TerminalColorCapability::TrueColor);
    CHECK(settled_live.foreground(ThemeToken::Accent, "x") == "\x1b[38;5;5mx\x1b[39m");
    // A non-faint token of a theme that carries a dim list stays unaffected.
    const auto rgb_theme = [&] {
        ResolvedTheme theme = settled;
        theme.dim.clear();
        theme.colors[static_cast<std::size_t>(ThemeToken::Muted)] = RgbThemeColor{.red = 1, .green = 2, .blue = 3};
        return theme;
    }();
    LiveTheme rgb_live(rgb_theme, tui::TerminalColorCapability::TrueColor);
    CHECK(rgb_live.foreground(ThemeToken::Muted, "x") == "\x1b[38;2;1;2;3mx\x1b[39m");
}

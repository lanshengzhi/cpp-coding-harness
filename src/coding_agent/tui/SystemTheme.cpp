#include "SystemTheme.hpp"

#include <cch/support/Error.hpp>
#include <cch/tui/Utils.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace cch::coding_agent::tui {
namespace {

// ============================================================================
// Recipe: color families and their tokens (frozen system-theme.ts, verbatim)
// ============================================================================

enum class FamilyName : std::size_t {
    Neutral,
    Blue,
    Green,
    Red,
    Yellow,
    Orange,
    Violet,
    Calamine,
    ThinkingSlate,
    ThinkingBlue,
    ThinkingPeriwinkle,
    ThinkingViolet,
    ThinkingMagenta,
    ThinkingRed,
};

/// A family's OKHSL hue and saturation: `max` at mid lightness, falling
/// toward `min` at black and white (pi `Family`).
struct Family {
    double hue;
    double saturation_min;
    double saturation_max;
    int slot;
};

constexpr Family kFamilies[] = {
        {231.49, 0.02, 0.08, 8}, // Neutral
        {231.49, 0.1, 0.68, 4},  // Blue
        {158.68, 0.1, 0.76, 2},  // Green
        {20.0, 0.1, 0.92, 1},    // Red
        {82.36, 0.5, 1.0, 3},    // Yellow
        {52.0, 0.12, 0.85, 3},   // Orange
        {295.0, 0.2, 0.6, 5},    // Violet
        {202.43, 0.1, 0.74, 6},  // Calamine
        {231.49, 0.08, 0.2, 4},  // ThinkingSlate
        {231.49, 0.2, 0.45, 4},  // ThinkingBlue
        {263.25, 0.3, 0.6, 6},   // ThinkingPeriwinkle
        {295.0, 0.4, 0.75, 5},   // ThinkingViolet
        {337.5, 0.5, 0.85, 13},  // ThinkingMagenta
        {20.0, 0.95, 1.0, 1},    // ThinkingRed
};

constexpr std::size_t family_index(FamilyName name) { return static_cast<std::size_t>(name); }

struct TokenFamily {
    ThemeToken token;
    FamilyName family;
};

// pi `TOKEN_FAMILIES` in the frozen literal order. The order is observable:
// it is the iteration order of the indexed tier's `dim` list.
constexpr TokenFamily kTokenFamilies[] = {
        {ThemeToken::SelectedBg, FamilyName::Blue},
        {ThemeToken::SearchMatchBg, FamilyName::Orange},
        {ThemeToken::UserMessageBg, FamilyName::Blue},
        {ThemeToken::CustomMessageBg, FamilyName::Violet},
        {ThemeToken::ToolPendingBg, FamilyName::Neutral},
        {ThemeToken::ToolSuccessBg, FamilyName::Green},
        {ThemeToken::ToolErrorBg, FamilyName::Red},
        {ThemeToken::Text, FamilyName::Neutral},
        {ThemeToken::UserMessageText, FamilyName::Neutral},
        {ThemeToken::CustomMessageText, FamilyName::Neutral},
        {ThemeToken::ToolTitle, FamilyName::Neutral},
        {ThemeToken::SyntaxOperator, FamilyName::Neutral},
        {ThemeToken::SyntaxPunctuation, FamilyName::Neutral},
        {ThemeToken::Muted, FamilyName::Neutral},
        {ThemeToken::Dim, FamilyName::Neutral},
        {ThemeToken::ThinkingText, FamilyName::Neutral},
        {ThemeToken::ToolOutput, FamilyName::Neutral},
        {ThemeToken::MdLinkUrl, FamilyName::Neutral},
        {ThemeToken::MdQuote, FamilyName::Neutral},
        {ThemeToken::MdQuoteBorder, FamilyName::Neutral},
        {ThemeToken::MdHr, FamilyName::Neutral},
        {ThemeToken::MdCodeBlockBorder, FamilyName::Neutral},
        {ThemeToken::ToolDiffContext, FamilyName::Neutral},
        {ThemeToken::SyntaxComment, FamilyName::Neutral},
        {ThemeToken::ScrollbarTrack, FamilyName::Neutral},
        {ThemeToken::ScrollbarThumb, FamilyName::Neutral},
        {ThemeToken::SearchMatchText, FamilyName::Neutral},
        {ThemeToken::BorderMuted, FamilyName::Neutral},
        {ThemeToken::Accent, FamilyName::Violet},
        {ThemeToken::BorderAccent, FamilyName::Violet},
        {ThemeToken::CustomMessageLabel, FamilyName::Violet},
        {ThemeToken::MdCode, FamilyName::Violet},
        {ThemeToken::MdListBullet, FamilyName::Violet},
        {ThemeToken::SyntaxType, FamilyName::Violet},
        {ThemeToken::Border, FamilyName::Blue},
        {ThemeToken::MdLink, FamilyName::Blue},
        {ThemeToken::SyntaxKeyword, FamilyName::Blue},
        {ThemeToken::SyntaxVariable, FamilyName::Calamine},
        {ThemeToken::Success, FamilyName::Green},
        {ThemeToken::MdCodeBlock, FamilyName::Green},
        {ThemeToken::ToolDiffAdded, FamilyName::Green},
        {ThemeToken::BashMode, FamilyName::Green},
        {ThemeToken::SyntaxNumber, FamilyName::Green},
        {ThemeToken::Error, FamilyName::Red},
        {ThemeToken::ToolDiffRemoved, FamilyName::Red},
        {ThemeToken::Warning, FamilyName::Yellow},
        {ThemeToken::MdHeading, FamilyName::Yellow},
        {ThemeToken::SyntaxFunction, FamilyName::Yellow},
        {ThemeToken::SyntaxString, FamilyName::Orange},
        {ThemeToken::ThinkingOff, FamilyName::Neutral},
        {ThemeToken::ThinkingMinimal, FamilyName::ThinkingSlate},
        {ThemeToken::ThinkingLow, FamilyName::ThinkingBlue},
        {ThemeToken::ThinkingMedium, FamilyName::ThinkingPeriwinkle},
        {ThemeToken::ThinkingHigh, FamilyName::ThinkingViolet},
        {ThemeToken::ThinkingXhigh, FamilyName::ThinkingMagenta},
        {ThemeToken::ThinkingMax, FamilyName::ThinkingRed},
};

static_assert(std::size(kTokenFamilies) == kThemeTokenCount);

/// pi `TEXT_MINIMUM_WCAG_CONTRAST`: the WCAG 2 contrast ratio body text
/// must reach on the surfaces it is drawn on.
constexpr double kTextMinimumWcagContrast = 4.5;

[[nodiscard]] FamilyName family_of(ThemeToken token) {
    for (const auto& entry : kTokenFamilies) {
        if (entry.token == token) return entry.family;
    }
    std::terminate();
}

[[nodiscard]] constexpr bool is_panel_token(ThemeToken token) {
    switch (token) {
    case ThemeToken::UserMessageBg:
    case ThemeToken::ToolPendingBg:
    case ThemeToken::ToolSuccessBg:
    case ThemeToken::ToolErrorBg:
    case ThemeToken::SelectedBg:
    case ThemeToken::SearchMatchBg:
    case ThemeToken::CustomMessageBg:
        return true;
    default:
        return false;
    }
}

/// pi `TOKEN_SLOTS`: palette slots for tokens that would otherwise share a
/// hue with a similar token (otherwise the family's own slot).
[[nodiscard]] constexpr int palette_slot(ThemeToken token, FamilyName family) {
    switch (token) {
    case ThemeToken::SyntaxString:
        return 2;
    case ThemeToken::SyntaxNumber:
        return 5;
    case ThemeToken::SearchMatchBg:
        return 3;
    default:
        return kFamilies[family_index(family)].slot;
    }
}

[[nodiscard]] constexpr std::size_t token_index(ThemeToken token) { return static_cast<std::size_t>(token); }

// ============================================================================
// Contrast levels and rules (frozen LEVELS/RULES, verbatim)
// ============================================================================

enum class Level : std::size_t {
    Panel,
    Track,
    Thinking0,
    Thinking1,
    Thinking2,
    Thinking3,
    Thinking4,
    Thinking5,
    Thinking6,
    Subtle,
    Thumb,
    Readable,
    Emphasis,
    TextOnPanel,
    Text,
};

constexpr std::size_t kLevelCount = 15;

/// Target-lightness curve: a polynomial in the surface's OKLab lightness
/// giving the OKLab lightness a token needs on it (pi `Curve`).
struct LevelCurve {
    std::array<double, 6> coefficients;
    double reachable_min;
    double reachable_max;
};

// pi `LEVELS` verbatim; even index dark, odd index light.
constexpr LevelCurve kLevelCurves[] = {
        // Panel
        {{0.29131, -0.39746, 2.33185, -0.85524, -1.2076, 0.86276}, 0.0, 0.979},
        {{-3.74073, 27.94549, -78.44258, 112.6798, -79.60015, 22.11277}, 0.348, 1.0},
        // Track
        {{0.39028, -0.23015, 0.83573, 2.43829, -4.38292, 2.01582}, 0.0, 0.946},
        {{-5.24921, 38.37322, -107.28833, 152.10005, -106.17127, 29.18061}, 0.368, 1.0},
        // Thinking0
        {{0.52988, -0.05809, -0.30924, 4.63567, -6.52933, 2.89108}, 0.0, 0.873},
        {{-28.27749, 182.85284, -469.62416, 603.15916, -384.59976, 97.35147}, 0.51, 1.0},
        // Thinking1
        {{0.55278, -0.03667, -0.45659, 4.95347, -6.90265, 3.0706}, 0.0, 0.858},
        {{-37.10484, 235.86282, -596.62344, 754.3633, -474.00763, 118.3551}, 0.535, 1.0},
        // Thinking2
        {{0.57486, -0.01765, -0.58987, 5.25227, -7.27175, 3.25532}, 0.0, 0.842},
        {{-59.89653, 377.05024, -945.07843, 1182.03145, -734.96375, 181.68658}, 0.556, 1.0},
        // Thinking3
        {{0.59621, -0.00062, -0.71148, 5.53588, -7.6392, 3.44606}, 0.0, 0.827},
        {{-72.07122, 445.84082, -1099.57352, 1353.88793, -829.53392, 202.26164}, 0.58, 1.0},
        // Thinking4
        {{0.61691, 0.01462, -0.82288, 5.80651, -8.00641, 3.64333}, 0.0, 0.811},
        {{-110.14338, 674.21488, -1645.75941, 2004.32367, -1215.15899, 293.3183}, 0.6, 1.0},
        // Thinking5
        {{0.63702, 0.02826, -0.92498, 6.06465, -8.37246, 3.84651}, 0.0, 0.795},
        {{-175.47701, 1063.54495, -2570.70594, 3098.80776, -1860.15527, 444.76392}, 0.62, 1.0},
        // Thinking6
        {{0.65658, 0.04044, -1.01835, 6.30989, -8.73529, 4.05439}, 0.0, 0.779},
        {{-183.81712, 1094.70055, -2602.68539, 3088.71276, -1826.91131, 430.75931}, 0.643, 1.0},
        // Subtle
        {{0.56762, -0.02475, -0.5383, 5.12628, -7.10931, 3.17324}, 0.0, 0.848},
        {{-232.85459, 1376.54473, -3249.11801, 3827.91186, -2248.29472, 526.55751}, 0.657, 1.0},
        // Thumb
        {{0.60323, 0.00278, -0.73328, 5.57157, -7.68067, 3.46933}, 0.0, 0.823},
        {{-82.89897, 511.01355, -1255.98095, 1540.76821, -940.68087, 228.58523}, 0.586, 1.0},
        // Readable
        {{0.66937, 0.04704, -1.06871, 6.43941, -8.9332, 4.17229}, 0.0, 0.77},
        {{-1554.52576, 8733.56817, -19604.93507, 21977.72696, -12300.99599, 2749.81288}, 0.751, 1.0},
        // Emphasis
        {{0.7303, 0.07695, -1.31626, 7.1681, -10.14436, 4.92846}, 0.0, 0.712},
        {{-4948.31942, 26870.91986, -58334.48399, 63280.17197, -34298.01053, 7430.30146}, 0.811, 1.0},
        // TextOnPanel
        {{0.86713, 0.05232, -0.89428, 4.79014, -5.5432, 1.75023}, 0.0, 0.542},
        {{-8570.89457, 43954.60805, -90084.00702, 92220.6791, -47152.15802, 9632.27113}, 0.867, 1.0},
        // Text
        {{0.89242, 0.02311, -0.44862, 2.34417, -0.06084, -2.63844}, 0.0, 0.5},
        {{-2004.67048, 6664.47299, -6060.70202, -1792.61209, 5133.82359, -1939.85583}, 0.894, 1.0},
};

static_assert(std::size(kLevelCurves) == kLevelCount * 2);

[[nodiscard]] constexpr const LevelCurve& level_curve(Level level, TerminalAppearance appearance) {
    const std::size_t appearance_index = appearance == TerminalAppearance::Light ? 1 : 0;
    return kLevelCurves[2 * static_cast<std::size_t>(level) + appearance_index];
}

/// A surface a rule draws a token on: the terminal background or another
/// token (pi `Surface` = `ThemeBg | "background" | "scrollbarTrack"`).
struct Surface {
    ThemeToken token;
    bool is_background;
};

constexpr Surface kBackgroundSurface{ThemeToken::Accent, true};
constexpr Surface kBackgroundOnly[] = {kBackgroundSurface};
constexpr Surface kSelectedBgSurface[] = {{ThemeToken::SelectedBg, false}};
constexpr Surface kUserMessageBgSurface[] = {{ThemeToken::UserMessageBg, false}};
constexpr Surface kSearchMatchBgSurface[] = {{ThemeToken::SearchMatchBg, false}};
constexpr Surface kScrollbarTrackSurface[] = {{ThemeToken::ScrollbarTrack, false}};
constexpr Surface kToolPanelSurfaces[] = {
        {ThemeToken::ToolPendingBg, false},
        {ThemeToken::ToolSuccessBg, false},
        {ThemeToken::ToolErrorBg, false},
};
constexpr Surface kAccentSurfaces[] = {
        kBackgroundSurface,
        {ThemeToken::SelectedBg, false},
        {ThemeToken::ToolPendingBg, false},
        {ThemeToken::ToolSuccessBg, false},
        {ThemeToken::ToolErrorBg, false},
};
constexpr Surface kMutedSurfaces[] = {
        kBackgroundSurface,
        {ThemeToken::SelectedBg, false},
        {ThemeToken::CustomMessageBg, false},
        {ThemeToken::ToolPendingBg, false},
        {ThemeToken::ToolSuccessBg, false},
        {ThemeToken::ToolErrorBg, false},
};
constexpr Surface kCustomMessageToolSurfaces[] = {
        {ThemeToken::CustomMessageBg, false},
        {ThemeToken::ToolPendingBg, false},
        {ThemeToken::ToolSuccessBg, false},
        {ThemeToken::ToolErrorBg, false},
};
constexpr Surface kLabelSurfaces[] = {
        kBackgroundSurface,
        {ThemeToken::CustomMessageBg, false},
        {ThemeToken::SelectedBg, false},
        {ThemeToken::ToolPendingBg, false},
        {ThemeToken::ToolSuccessBg, false},
        {ThemeToken::ToolErrorBg, false},
};
constexpr Surface kToolOutputSurfaces[] = {
        kBackgroundSurface,
        {ThemeToken::ToolPendingBg, false},
        {ThemeToken::ToolSuccessBg, false},
        {ThemeToken::ToolErrorBg, false},
};
constexpr Surface kMessageSurfaces[] = {
        kBackgroundSurface,
        {ThemeToken::UserMessageBg, false},
        {ThemeToken::CustomMessageBg, false},
};
constexpr Surface kCodeBlockSurfaces[] = {
        kBackgroundSurface,
        {ThemeToken::UserMessageBg, false},
        {ThemeToken::CustomMessageBg, false},
        {ThemeToken::ToolPendingBg, false},
        {ThemeToken::ToolSuccessBg, false},
        {ThemeToken::ToolErrorBg, false},
};

struct Rule {
    ThemeToken token;
    std::span<const Surface> on;
    Level level;
};

constexpr Rule kRules[] = {
        // ...each(PANELS, ["background"], "panel")
        {ThemeToken::UserMessageBg, kBackgroundOnly, Level::Panel},
        {ThemeToken::ToolPendingBg, kBackgroundOnly, Level::Panel},
        {ThemeToken::ToolSuccessBg, kBackgroundOnly, Level::Panel},
        {ThemeToken::ToolErrorBg, kBackgroundOnly, Level::Panel},
        {ThemeToken::SelectedBg, kBackgroundOnly, Level::Panel},
        {ThemeToken::SearchMatchBg, kBackgroundOnly, Level::Panel},
        {ThemeToken::CustomMessageBg, kBackgroundOnly, Level::Panel},
        {ThemeToken::Text, kBackgroundOnly, Level::Text},
        {ThemeToken::Text, kSelectedBgSurface, Level::TextOnPanel},
        {ThemeToken::UserMessageText, kUserMessageBgSurface, Level::TextOnPanel},
        {ThemeToken::ToolTitle, kToolPanelSurfaces, Level::TextOnPanel},
        // ...each(["accent", "success", "error", "warning"], ...)
        {ThemeToken::Accent, kAccentSurfaces, Level::Readable},
        {ThemeToken::Success, kAccentSurfaces, Level::Readable},
        {ThemeToken::Error, kAccentSurfaces, Level::Readable},
        {ThemeToken::Warning, kAccentSurfaces, Level::Readable},
        {ThemeToken::Muted, kMutedSurfaces, Level::Readable},
        {ThemeToken::Dim, kMutedSurfaces, Level::Subtle},
        {ThemeToken::ThinkingText, kBackgroundOnly, Level::Readable},
        {ThemeToken::CustomMessageText, kCustomMessageToolSurfaces, Level::Readable},
        {ThemeToken::CustomMessageLabel, kLabelSurfaces, Level::Readable},
        {ThemeToken::ToolOutput, kToolOutputSurfaces, Level::Readable},
        // ...each(["mdHeading", "mdLink", "mdLinkUrl", "mdCode", "mdQuote",
        //          "mdCodeBlockBorder", "mdListBullet"], ...)
        {ThemeToken::MdHeading, kMessageSurfaces, Level::Readable},
        {ThemeToken::MdLink, kMessageSurfaces, Level::Readable},
        {ThemeToken::MdLinkUrl, kMessageSurfaces, Level::Readable},
        {ThemeToken::MdCode, kMessageSurfaces, Level::Readable},
        {ThemeToken::MdQuote, kMessageSurfaces, Level::Readable},
        {ThemeToken::MdCodeBlockBorder, kMessageSurfaces, Level::Readable},
        {ThemeToken::MdListBullet, kMessageSurfaces, Level::Readable},
        {ThemeToken::MdCodeBlock, kCodeBlockSurfaces, Level::Readable},
        // ...each(["toolDiffAdded", "toolDiffRemoved", "toolDiffContext"], ...)
        {ThemeToken::ToolDiffAdded, kToolOutputSurfaces, Level::Readable},
        {ThemeToken::ToolDiffRemoved, kToolOutputSurfaces, Level::Readable},
        {ThemeToken::ToolDiffContext, kToolOutputSurfaces, Level::Readable},
        // ...each(["syntaxComment", "syntaxKeyword", "syntaxFunction",
        //          "syntaxVariable", "syntaxString", "syntaxNumber",
        //          "syntaxType", "syntaxOperator", "syntaxPunctuation"], ...)
        {ThemeToken::SyntaxComment, kCodeBlockSurfaces, Level::Readable},
        {ThemeToken::SyntaxKeyword, kCodeBlockSurfaces, Level::Readable},
        {ThemeToken::SyntaxFunction, kCodeBlockSurfaces, Level::Readable},
        {ThemeToken::SyntaxVariable, kCodeBlockSurfaces, Level::Readable},
        {ThemeToken::SyntaxString, kCodeBlockSurfaces, Level::Readable},
        {ThemeToken::SyntaxNumber, kCodeBlockSurfaces, Level::Readable},
        {ThemeToken::SyntaxType, kCodeBlockSurfaces, Level::Readable},
        {ThemeToken::SyntaxOperator, kCodeBlockSurfaces, Level::Readable},
        {ThemeToken::SyntaxPunctuation, kCodeBlockSurfaces, Level::Readable},
        {ThemeToken::SearchMatchText, kSearchMatchBgSurface, Level::Readable},
        // ...each(["bashMode", "border", "borderAccent"], ["background"])
        {ThemeToken::BashMode, kBackgroundOnly, Level::Readable},
        {ThemeToken::Border, kBackgroundOnly, Level::Readable},
        {ThemeToken::BorderAccent, kBackgroundOnly, Level::Readable},
        {ThemeToken::BorderMuted, kBackgroundOnly, Level::Subtle},
        // ...each(["mdQuoteBorder", "mdHr"], ...)
        {ThemeToken::MdQuoteBorder, kCodeBlockSurfaces, Level::Readable},
        {ThemeToken::MdHr, kCodeBlockSurfaces, Level::Readable},
        {ThemeToken::ScrollbarTrack, kBackgroundOnly, Level::Track},
        {ThemeToken::ScrollbarThumb, kScrollbarTrackSurface, Level::Thumb},
        // THINKING.map((token, index) => ({ token, on: ["background"], level: THINKING_LEVELS[index] }))
        {ThemeToken::ThinkingOff, kBackgroundOnly, Level::Thinking0},
        {ThemeToken::ThinkingMinimal, kBackgroundOnly, Level::Thinking1},
        {ThemeToken::ThinkingLow, kBackgroundOnly, Level::Thinking2},
        {ThemeToken::ThinkingMedium, kBackgroundOnly, Level::Thinking3},
        {ThemeToken::ThinkingHigh, kBackgroundOnly, Level::Thinking4},
        {ThemeToken::ThinkingXhigh, kBackgroundOnly, Level::Thinking5},
        {ThemeToken::ThinkingMax, kBackgroundOnly, Level::Thinking6},
};

/// pi `FOREGROUND_TOKENS`: text-level tokens that take the terminal's
/// foreground.
constexpr ThemeToken kForegroundTokens[] = {
        ThemeToken::Text,
        ThemeToken::UserMessageText,
        ThemeToken::ToolTitle,
};

[[nodiscard]] constexpr bool is_foreground_token(ThemeToken token) {
    for (const auto foreground : kForegroundTokens) {
        if (foreground == token) return true;
    }
    return false;
}

/// pi `SOLVE_ORDER`: topological — every surface before the tokens drawn on
/// it — derived from RULES exactly like the frozen module-init IIFE.
[[nodiscard]] const std::vector<ThemeToken>& solve_order() {
    static const std::vector<ThemeToken> order = [] {
        std::vector<ThemeToken> result;
        const auto visit = [&](auto&& self, ThemeToken token) -> void {
            if (std::ranges::find(result, token) != result.end()) return;
            for (const auto& rule : kRules) {
                if (rule.token != token) continue;
                for (const Surface surface : rule.on) {
                    if (surface.is_background) continue;
                    self(self, surface.token);
                }
            }
            result.push_back(token);
        };
        for (const auto& rule : kRules)
            visit(visit, rule.token);
        return result;
    }();
    return order;
}

// ============================================================================
// Color helpers
// ============================================================================

/// OKLab lightness of an sRGB color, 0-1 (pi `oklabLightness`).
[[nodiscard]] double oklab_lightness(const cch::tui::RgbColor& color) {
    const auto converted = cch::tui::color_to_oklch(cch::tui::Color{color});
    return converted ? converted->l : 0.0;
}

/// OKHSL lightness of an OKLab lightness. Re-derived in this Owner from
/// Björn Ottosson's OKHSL map (the same three lines as `cch_tui`'s
/// file-local `oklab_to_okhsl_lightness`, duplicated here so the Owner
/// Interface stays unchanged).
[[nodiscard]] double oklab_to_okhsl_lightness(double value) {
    constexpr double kK1 = 0.206;
    constexpr double kK2 = 0.03;
    constexpr double kK3 = (1.0 + kK1) / (1.0 + kK2);
    const double term = kK3 * value - kK1;
    return 0.5 * (term + std::sqrt(term * term + 4.0 * kK2 * kK3 * value));
}

[[nodiscard]] cch::tui::RgbColor okhsl(double hue, double saturation, double lightness) {
    auto color = cch::tui::okhsl_to_rgb(hue, saturation, lightness);
    if (!color) std::terminate();
    return *color;
}

/// Saturation weight at a lightness: a Gaussian (center 0.5, sigma 0.25), 0
/// at black and white, 1 in the middle (pi `bellWeight`).
[[nodiscard]] double bell_weight(double lightness) {
    const auto gaussian = [](double x) { return std::exp(-((x - 0.5) * (x - 0.5)) / (2.0 * 0.25 * 0.25)); };
    return (gaussian(lightness) - gaussian(0.0)) / (1.0 - gaussian(0.0));
}

/// A family's saturation curve relative to its maximum: 1 at mid lightness,
/// `min / max` at black and white (pi `saturationCurve`).
[[nodiscard]] double saturation_curve(const Family& family, double lightness) {
    const double floor = family.saturation_max > 0 ? family.saturation_min / family.saturation_max : 1.0;
    return floor + (1.0 - floor) * bell_weight(lightness);
}

/// The target lightness for a level on a surface, or nothing where the level
/// cannot be reached (pi `levelTarget`).
[[nodiscard]] std::optional<double> level_target(Level level, TerminalAppearance appearance, double surface_lightness) {
    const auto& curve = level_curve(level, appearance);
    if (surface_lightness < curve.reachable_min || surface_lightness > curve.reachable_max) return std::nullopt;
    double sum = 0.0;
    for (std::size_t power = 0; power < curve.coefficients.size(); ++power) {
        sum += curve.coefficients[power] * std::pow(surface_lightness, static_cast<double>(power));
    }
    return sum;
}

/// A terminal color's OKHSL channels and its OKLCH chroma (pi `SourceColor`).
struct SourceColor {
    double h;
    double s;
    double l;
    double chroma;
};

[[nodiscard]] SourceColor source_of(const cch::tui::RgbColor& color) {
    const auto channels = cch::tui::color_to_okhsl(cch::tui::Color{color});
    const auto oklch = cch::tui::color_to_oklch(cch::tui::Color{color});
    if (!channels || !oklch) std::terminate();
    return {.h = channels->h, .s = channels->s, .l = channels->l, .chroma = oklch->c};
}

/// A source color's hue at another OKHSL lightness. Its saturation applies
/// at its own lightness and falls off toward black and white along the
/// family's saturation curve, never rising above it; chroma is also capped
/// at the source's so pastel palettes stay pastel (pi `anchored`).
[[nodiscard]] cch::tui::RgbColor anchored(
        const SourceColor& source, const Family& family, double lightness, double saturation) {
    const double anchor = saturation_curve(family, source.l);
    const double falloff = anchor > 0 ? std::min(1.0, saturation_curve(family, lightness) / anchor) : 1.0;
    const auto color = okhsl(source.h, source.s * falloff * saturation, lightness);
    const double cap = source.chroma * falloff * saturation;
    const auto lch = cch::tui::color_to_oklch(cch::tui::Color{color});
    if (!lch) std::terminate();
    if (lch->c <= cap) return color;
    auto capped = cch::tui::oklch_color(lch->l, cap, source.h);
    if (!capped) std::terminate();
    auto rgb = cch::tui::color_to_rgb(cch::tui::Color{*capped});
    if (!rgb) std::terminate();
    return *rgb;
}

/// Move a text color toward white or black until it reaches the WCAG
/// minimum on every surface (pi `withTextContrast`).
[[nodiscard]] cch::tui::RgbColor with_text_contrast(
        const cch::tui::RgbColor& color, const std::vector<cch::tui::RgbColor>& surfaces, bool lighter) {
    const auto meets = [&surfaces](const cch::tui::RgbColor& candidate) {
        for (const auto& surface : surfaces) {
            if (wcag_contrast(candidate, surface) < kTextMinimumWcagContrast) return false;
        }
        return true;
    };
    if (meets(color)) return color;
    const auto channels = cch::tui::color_to_okhsl(cch::tui::Color{color});
    if (!channels) std::terminate();
    const auto at = [&](double lightness) { return okhsl(channels->h, channels->s, lightness); };
    const double extreme = lighter ? 1.0 : 0.0;
    if (!meets(at(extreme))) return at(extreme);
    double low = channels->l;
    double high = extreme;
    for (int index = 0; index < 20; ++index) {
        const double middle = (low + high) / 2;
        if (meets(at(middle))) {
            high = middle;
        } else {
            low = middle;
        }
    }
    return at(high);
}

// ============================================================================
// Generation (frozen generateSystemThemeColors)
// ============================================================================

struct SolvedColors {
    cch::tui::RgbColor background;
    std::map<ThemeToken, cch::tui::RgbColor> tokens;
};

/// The relaxation floor: dark keeps `readable`, light compresses toward
/// `subtle` (pi `READABLE_FLOOR`).
[[nodiscard]] constexpr Level readable_floor(bool lighter) { return lighter ? Level::Readable : Level::Subtle; }

class SystemThemeGenerator {
public:
    SystemThemeGenerator(double saturation,
            TerminalAppearance appearance,
            cch::tui::RgbColor background,
            const std::vector<SourceColor>* palette)
        : saturation_(saturation), appearance_(appearance), lighter_(appearance == TerminalAppearance::Dark),
          extreme_(lighter_ ? 1.0 : 0.0), background_(background), background_lightness_(oklab_lightness(background)),
          palette_(palette) {}

    /// A token's color at an OKLab lightness (pi `paint`).
    [[nodiscard]] cch::tui::RgbColor paint(ThemeToken token, double oklab_lightness) const {
        const double lightness = oklab_to_okhsl_lightness(oklab_lightness);
        const FamilyName family_name = family_of(token);
        const Family& family = kFamilies[family_index(family_name)];
        if (palette_ == nullptr) {
            const double saturation =
                    (family.saturation_min + (family.saturation_max - family.saturation_min) * bell_weight(lightness)) *
                    saturation_;
            return okhsl(family.hue, saturation, lightness);
        }
        return anchored(palette_->at(static_cast<std::size_t>(palette_slot(token, family_name))),
                family,
                lightness,
                saturation_);
    }

    /// The lightness a rule needs on a surface, relaxed by `t` (pi `target`).
    [[nodiscard]] std::optional<double> target(Level level, double surface_lightness, double t) const {
        const auto reached = level_target(level, appearance_, surface_lightness);
        if (!reached.has_value() && t == 0.0) return std::nullopt;
        const double distance = (reached.has_value() ? *reached : extreme_) - surface_lightness;
        const auto floor_reached = level_target(readable_floor(lighter_), appearance_, surface_lightness);
        const double floor = (floor_reached.has_value() ? *floor_reached : extreme_) - surface_lightness;
        const double compressed =
                std::abs(distance) > std::abs(floor) ? distance - (distance - floor) * std::min(t, 1.0) : distance;
        return surface_lightness + compressed * (1.0 - std::max(0.0, t - 1.0));
    }

    /// Keep a panel light enough (or dark enough) that white (or black) text
    /// still reaches the body text minimum on it (pi `limitPanel`).
    [[nodiscard]] cch::tui::RgbColor limit_panel(ThemeToken token, double l) const {
        const auto color = paint(token, l);
        if (readable(color)) return color;
        double low = background_lightness_;
        double high = l;
        for (int index = 0; index < 20; ++index) {
            const double middle = (low + high) / 2;
            if (readable(paint(token, middle))) {
                low = middle;
            } else {
                high = middle;
            }
        }
        return paint(token, low);
    }

    /// Solve every token at relaxation `t`; fails when any target falls
    /// outside [0, 1] (pi `solve`).
    [[nodiscard]] std::optional<SolvedColors> solve(double t) const {
        SolvedColors colors{.background = background_, .tokens = {}};
        for (const ThemeToken token : solve_order()) {
            std::vector<double> targets;
            for (const auto& rule : kRules) {
                if (rule.token != token) continue;
                for (const Surface surface : rule.on) {
                    const auto value = target(rule.level, oklab_lightness(surface_color(colors, surface)), t);
                    if (!value.has_value() || *value < 0.0 || *value > 1.0) return std::nullopt;
                    targets.push_back(*value);
                }
            }
            const double l = lighter_ ? *std::max_element(targets.begin(), targets.end())
                                      : *std::min_element(targets.begin(), targets.end());
            colors.tokens[token] = is_panel_token(token) ? limit_panel(token, l) : paint(token, l);
        }
        return colors;
    }

    [[nodiscard]] bool lighter() const { return lighter_; }
    [[nodiscard]] double extreme() const { return extreme_; }
    [[nodiscard]] double saturation() const { return saturation_; }

private:
    [[nodiscard]] bool readable(const cch::tui::RgbColor& color) const {
        const cch::tui::RgbColor extreme_text = lighter_ ? cch::tui::RgbColor{.red = 255, .green = 255, .blue = 255}
                                                         : cch::tui::RgbColor{.red = 0, .green = 0, .blue = 0};
        return wcag_contrast(extreme_text, color) >= kTextMinimumWcagContrast;
    }

    [[nodiscard]] const cch::tui::RgbColor& surface_color(const SolvedColors& colors, Surface surface) const {
        if (surface.is_background) return colors.background;
        const auto found = colors.tokens.find(surface.token);
        // SOLVE_ORDER places every surface before the tokens drawn on it.
        if (found == colors.tokens.end()) std::terminate();
        return found->second;
    }

    double saturation_;
    TerminalAppearance appearance_;
    bool lighter_;
    double extreme_;
    cch::tui::RgbColor background_;
    double background_lightness_;
    const std::vector<SourceColor>* palette_;
};

[[nodiscard]] ResolvedThemeColor resolved_color_of(const cch::tui::RgbColor& color) {
    return RgbThemeColor{
            .red = static_cast<std::uint8_t>(std::lround(color.red)),
            .green = static_cast<std::uint8_t>(std::lround(color.green)),
            .blue = static_cast<std::uint8_t>(std::lround(color.blue)),
    };
}

/// Colors for terminals that reported nothing: the terminal renders ANSI
/// indices 0-15 and the default colors with its own theme; neutral tokens
/// below body text are faint (SGR 2) instead of bright black (pi
/// `indexedColors`).
[[nodiscard]] SystemThemeColors indexed_colors(double saturation, std::optional<TerminalAppearance> appearance) {
    SystemThemeColors result;
    result.appearance = appearance;
    for (const auto& [token, family] : kTokenFamilies) {
        const std::size_t index = token_index(token);
        if (is_panel_token(token)) {
            result.colors[index] = TerminalDefaultThemeColor{};
            continue;
        }
        const bool neutral = family == FamilyName::Neutral;
        if (!neutral && saturation > 0) {
            result.colors[index] = XtermThemeColor{.index = static_cast<std::uint8_t>(palette_slot(token, family))};
        } else {
            result.colors[index] = TerminalDefaultThemeColor{};
        }
        if (neutral && !is_foreground_token(token)) result.dim.push_back(token);
    }
    return result;
}

} // namespace

SystemThemeColors generate_system_theme_colors(const SystemThemeInput& input) {
    const double saturation = std::clamp(input.saturation, 0.0, 1.0);
    if (!input.background.has_value()) return indexed_colors(saturation, input.appearance_hint);

    std::vector<SourceColor> palette;
    const std::vector<SourceColor>* palette_ptr = nullptr;
    if (input.palette.has_value() && input.palette->size() == 16) {
        palette.reserve(input.palette->size());
        for (const auto& color : *input.palette) {
            palette.push_back(source_of(color));
        }
        palette_ptr = &palette;
    }

    const TerminalAppearance appearance = terminal_appearance(*input.background, input.foreground);
    const SystemThemeGenerator generator(saturation, appearance, *input.background, palette_ptr);

    double relaxation = 0.0;
    auto colors = generator.solve(0.0);
    if (!colors.has_value()) {
        // Mid-gray backgrounds cannot fit every level: relax as little as
        // possible. Full relaxation always fits.
        double low = 0.0;
        double high = 2.0;
        colors = generator.solve(high);
        for (int index = 0; index < 20; ++index) {
            const double middle = (low + high) / 2;
            auto attempt = generator.solve(middle);
            if (attempt.has_value()) {
                high = middle;
                colors = std::move(attempt);
            } else {
                low = middle;
            }
        }
        relaxation = high;
    }
    const SolvedColors solved = colors.value_or(SolvedColors{.background = *input.background, .tokens = {}});

    const auto surface_color = [&solved](Surface surface) -> const cch::tui::RgbColor& {
        if (surface.is_background) return solved.background;
        const auto found = solved.tokens.find(surface.token);
        return found != solved.tokens.end() ? found->second : solved.background;
    };
    const auto surfaces_of = [&surface_color](ThemeToken token) {
        std::vector<cch::tui::RgbColor> surfaces;
        for (const auto& rule : kRules) {
            if (rule.token != token) continue;
            for (const Surface surface : rule.on) {
                surfaces.push_back(surface_color(surface));
            }
        }
        return surfaces;
    };

    SystemThemeColors result;
    result.appearance = appearance;
    for (const auto& [token, family] : kTokenFamilies) {
        (void)family;
        const auto found = solved.tokens.find(token);
        if (found == solved.tokens.end()) {
            result.colors[token_index(token)] = TerminalDefaultThemeColor{};
        } else {
            result.colors[token_index(token)] = resolved_color_of(found->second);
        }
    }

    // Body text uses the terminal's own foreground where it is clearly
    // stronger than muted text; otherwise the foreground's hue at just
    // enough lightness — and always at least 4.5:1 on its surfaces.
    for (const ThemeToken token : kForegroundTokens) {
        const auto surfaces = surfaces_of(token);
        const auto solved_token = solved.tokens.find(token);
        if (solved_token == solved.tokens.end()) continue;
        cch::tui::RgbColor text = solved_token->second;
        if (input.foreground.has_value()) {
            std::vector<double> targets;
            bool all_valid = true;
            for (const auto& surface : surfaces) {
                const auto value = generator.target(Level::Emphasis, oklab_lightness(surface), relaxation);
                if (!value.has_value() || *value < 0.0 || *value > 1.0) {
                    all_valid = false;
                    break;
                }
                targets.push_back(*value);
            }
            if (all_valid && !targets.empty()) {
                const double needed = generator.lighter() ? *std::max_element(targets.begin(), targets.end())
                                                          : *std::min_element(targets.begin(), targets.end());
                const double foreground_lightness = oklab_lightness(*input.foreground);
                if (generator.lighter() ? foreground_lightness >= needed : foreground_lightness <= needed) {
                    result.colors[token_index(token)] = TerminalDefaultThemeColor{};
                    continue;
                }
                text = anchored(source_of(*input.foreground),
                        kFamilies[family_index(FamilyName::Neutral)],
                        oklab_to_okhsl_lightness(needed),
                        saturation);
            }
        }
        result.colors[token_index(token)] = resolved_color_of(with_text_contrast(text, surfaces, generator.lighter()));
    }
    return result;
}

ResolvedTheme system_theme_from_colors(const SystemThemeColors& generated) {
    ResolvedTheme theme;
    theme.name = std::string{kSystemThemeName};
    theme.colors = generated.colors;
    theme.appearance = generated.appearance;
    theme.dim = generated.dim;
    return theme;
}

} // namespace cch::coding_agent::tui

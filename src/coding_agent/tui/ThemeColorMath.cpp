#include "ThemeColorMath.hpp"

#include <cch/tui/Utils.hpp>

#include <algorithm>
#include <cmath>

namespace cch::coding_agent::tui {
namespace {

constexpr double kTextMinimumWcagContrast = 4.5;

[[nodiscard]] double oklab_lightness(cch::tui::RgbColor color) {
    const auto converted = cch::tui::color_to_oklch(cch::tui::Color{color});
    return converted ? converted->l : 0.0;
}

} // namespace

double relative_luminance(cch::tui::RgbColor color) {
    const auto linear = [](double channel) {
        const auto value = channel / 255.0;
        return value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * linear(color.red) + 0.7152 * linear(color.green) + 0.0722 * linear(color.blue);
}

double wcag_contrast(cch::tui::RgbColor first, cch::tui::RgbColor second) {
    const auto first_luminance = relative_luminance(first);
    const auto second_luminance = relative_luminance(second);
    return (std::max(first_luminance, second_luminance) + 0.05) / (std::min(first_luminance, second_luminance) + 0.05);
}

TerminalAppearance terminal_appearance(cch::tui::RgbColor background, std::optional<cch::tui::RgbColor> foreground) {
    const cch::tui::RgbColor white{.red = 255, .green = 255, .blue = 255};
    const cch::tui::RgbColor black{.red = 0, .green = 0, .blue = 0};
    const auto white_contrast = wcag_contrast(white, background);
    const auto black_contrast = wcag_contrast(black, background);
    const auto background_lightness = oklab_lightness(background);
    if (foreground) {
        const auto foreground_lightness = oklab_lightness(*foreground);
        if (std::abs(foreground_lightness - background_lightness) > 0.05) {
            const auto appearance =
                    foreground_lightness > background_lightness ? TerminalAppearance::Dark : TerminalAppearance::Light;
            const auto best_contrast = appearance == TerminalAppearance::Dark ? white_contrast : black_contrast;
            if (best_contrast >= kTextMinimumWcagContrast) return appearance;
        }
    }
    return white_contrast >= black_contrast ? TerminalAppearance::Dark : TerminalAppearance::Light;
}

} // namespace cch::coding_agent::tui

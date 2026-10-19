#pragma once

#include <cch/tui/Utils.hpp>

#include <optional>

namespace cch::coding_agent::tui {

enum class TerminalAppearance {
    Dark,
    Light,
};

/// Product theme color math shared by terminal appearance selection and the
/// generated system theme implementation (#1032).
[[nodiscard]] double relative_luminance(cch::tui::RgbColor color);
[[nodiscard]] double wcag_contrast(cch::tui::RgbColor first, cch::tui::RgbColor second);
[[nodiscard]] TerminalAppearance terminal_appearance(
        cch::tui::RgbColor background, std::optional<cch::tui::RgbColor> foreground);

} // namespace cch::coding_agent::tui

#pragma once

#include "Theme.hpp"
#include "ThemeColorMath.hpp"

#include <array>
#include <cstddef>
#include <optional>
#include <string_view>
#include <vector>

namespace cch::coding_agent::tui {

/// pi `SYSTEM_THEME_NAME` (system-theme.ts): the reserved generated theme
/// name; `loadTheme` resolves it before registered themes and files.
inline constexpr std::string_view kSystemThemeName{"system"};

/// pi `SystemThemeInput` (system-theme.ts): the terminal's reported colors
/// plus the generator controls. `palette` is the ANSI 0-15 report; the
/// generator anchors hues to it only when it holds all 16 entries.
struct SystemThemeInput {
    std::optional<cch::tui::RgbColor> foreground{std::nullopt};
    std::optional<cch::tui::RgbColor> background{std::nullopt};
    std::optional<std::vector<cch::tui::RgbColor>> palette{std::nullopt};
    /// Saturation multiplier from 0 (grayscale, the pending state) to 1.
    double saturation{1.0};
    /// pi `appearanceHint`: the appearance to use when the terminal did not
    /// report its background (its mode-2031 report or COLORFGBG).
    std::optional<TerminalAppearance> appearance_hint{std::nullopt};
};

/// pi `SystemThemeColors` (system-theme.ts): the generated 56-token palette
/// (`""` → the terminal default, an xterm index → the terminal's own ANSI
/// palette), the faint-token set (`dim`, SGR 2 — the no-report tier) and the
/// generated appearance.
struct SystemThemeColors {
    std::array<ResolvedThemeColor, kThemeTokenCount> colors{};
    std::vector<ThemeToken> dim;
    std::optional<TerminalAppearance> appearance{std::nullopt};
};

/// pi `generateSystemThemeColors` (system-theme.ts): generate the system
/// theme from the terminal's reported colors. Without a reported background
/// the theme is the indexed tier: panel tokens are the terminal default,
/// non-neutral tokens take their family's ANSI slot (while saturation is
/// nonzero) and neutral tokens below body text are faint.
[[nodiscard]] SystemThemeColors generate_system_theme_colors(const SystemThemeInput& input);

/// pi `createSystemTheme` (theme.ts): the resolved `system` theme built from
/// generated colors (name `kSystemThemeName`, appearance and `dim` carried).
[[nodiscard]] ResolvedTheme system_theme_from_colors(const SystemThemeColors& generated);

} // namespace cch::coding_agent::tui

#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace cch::tui {

/// Options for the frozen pi `renderLatex` seam (pi `RenderLatexOptions`).
struct LatexOptions {
    /// Stack fractions and operator limits vertically for display math
    /// (default: false).
    ///
    /// The vertical display layout (stacked fractions, operator limit columns
    /// and script baselines) is owned by #974. Until that stage lands, the
    /// `true` value reports the frozen failure value instead of a partial
    /// formula.
    bool display{false};
};

/// Render a basic LaTeX math expression as terminal-friendly Unicode text.
/// Returns `std::nullopt` when the expression contains unsupported or malformed
/// syntax; the source is never dropped and no partial formula is produced
/// (frozen pi `renderLatex`, whose failure value is `undefined`).
///
/// Inline grammar is supported: named symbols, grouping, `\frac`, `\sqrt`,
/// accents, `\mathbb`, scripts, named and limit operators, spacing, wrappers
/// and `\begin{equation}`-style inline environments. Display environments
/// (`aligned`, `cases`, matrix families) belong to #975 and report the same
/// failure value.
[[nodiscard]] std::optional<std::string> render_latex(std::string_view source, const LatexOptions& options = {});

} // namespace cch::tui
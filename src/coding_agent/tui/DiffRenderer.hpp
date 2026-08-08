#pragma once

#include "coding_agent/tui/Theme.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace cch::coding_agent::tui {

/// Renders a display diff string with pi `diff.ts` semantics: context lines
/// in `toolDiffContext`, removed lines in `toolDiffRemoved` and added lines
/// in `toolDiffAdded` (with tabs expanded), and a single-line modification
/// highlighted word-level with inverse on changed tokens. The returned text
/// carries inline SGR styling and renders through cch::tui::Text.
[[nodiscard]] std::string render_diff(
    const LiveTheme& theme,
    std::string_view diff_text);

} // namespace cch::coding_agent::tui

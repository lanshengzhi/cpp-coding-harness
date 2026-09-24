#pragma once

// Every overlay must render within the width it is handed: the composed render
// path rejects a line whose visible width exceeds the bound and aborts the whole
// app on the first one (issue #426). Component tests sweep narrow widths so an
// untruncated row fails here instead of in a user's terminal (issue #790).

#include "support/RenderedScreen.hpp"

#include <cch/tui/Component.hpp>
#include <cch/tui/Utils.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <format>
#include <string>

namespace cch::tests {

/// Narrow widths every overlay must survive, including the 66-column split a
/// 116-column terminal produces.
inline constexpr std::size_t kNarrowOverlayWidths[] = {8, 16, 40, 66, 80};

/// Describe the first over-wide line, or return an empty string when every line
/// fits its bound.
[[nodiscard]] inline std::string over_wide_line(const cch::tui::RenderResult& rendered, std::size_t width) {
    for (const auto& line : rendered.lines) {
        const auto visible = cch::tui::visible_width(strip_ansi(line));
        if (visible > width) {
            return std::format("line visible width {} exceeds bound {}", visible, width);
        }
    }
    return {};
}

/// Assert that every emitted line fits the render width bound.
inline void check_all_lines_bounded(const cch::tui::RenderResult& rendered, std::size_t width) {
    REQUIRE_FALSE(rendered.lines.empty());
    const auto over_wide = over_wide_line(rendered, width);
    INFO("bound " << width << ": " << over_wide);
    CHECK(over_wide.empty());
}

} // namespace cch::tests

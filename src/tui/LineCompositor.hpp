#pragma once

#include <cch/support/Error.hpp>

#include <cstddef>
#include <string>
#include <string_view>

namespace cch::tui::detail {

/// Splice `replacement` over the base row's columns
/// `[column, column + columns)`, leaving a row exactly `total_width` cells
/// wide (pi `compositeTuiLine`, `tui.ts:415-444`).
///
/// Every cell outside the region keeps the base row's content; the replacement
/// is padded (or truncated) to the region; style state is closed per region and
/// reopened over the base's trailing columns, so a styled child cannot bleed
/// into its neighbors. The constrained layout frame and the overlay compositor
/// share this one splice.
[[nodiscard]] support::Expected<std::string> replace_line_region(std::string_view line,
        std::string_view replacement,
        std::size_t column,
        std::size_t columns,
        std::size_t total_width);

} // namespace cch::tui::detail

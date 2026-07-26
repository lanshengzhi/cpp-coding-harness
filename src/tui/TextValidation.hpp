#pragma once

#include <cch/util/Error.hpp>

#include <string_view>

namespace cch::tui::detail {

// Issue #46 adds Unicode display-width and styling support. Until then, reject
// non-ASCII bytes at the TUI seam rather than split UTF-8 or drift rows.
[[nodiscard]] inline util::ExpectedVoid validate_ascii_text(std::string_view text) {
    for (const auto character : text) {
        if (static_cast<unsigned char>(character) > 0x7f) {
            return std::unexpected(util::make_error(
                util::ErrorCode::Validation,
                "TUI text layout does not support non-ASCII input",
                "Unicode display-width handling is unavailable"));
        }
    }
    return {};
}

} // namespace cch::tui::detail

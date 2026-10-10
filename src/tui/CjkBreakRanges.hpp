#pragma once

#include <algorithm>
#include <cstddef>
#include <iterator>

namespace cch::tui::detail {

// One closed code-point range of pi's CJK line-break set.
struct CjkBreakRange {
    char32_t first{0};
    char32_t last{0};
};

/// pi's CJK break code points as generated range data: utf8proc exposes no
/// Script_Extensions property, so the set is carried in
/// `tui/CjkBreakRanges.inc` rather than re-derived here. The generated include
/// records pi's regex text, its frozen revision, and its generator command.
inline constexpr CjkBreakRange kCjkBreakRanges[]{
#define CCH_CJK_BREAK_RANGE(range_first, range_last) {.first = range_first, .last = range_last},
#include "tui/CjkBreakRanges.inc"
#undef CCH_CJK_BREAK_RANGE
};

/// True when the code point is in pi's CJK break set. The generated ranges are
/// sorted and non-overlapping, so the first one ending at or after the code
/// point is the only candidate.
[[nodiscard]] inline bool is_cjk_break_codepoint(char32_t codepoint) {
    const auto range = std::ranges::lower_bound(kCjkBreakRanges, codepoint, {}, &CjkBreakRange::last);
    return range != std::end(kCjkBreakRanges) && codepoint >= range->first;
}

} // namespace cch::tui::detail

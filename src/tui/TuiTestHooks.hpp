#pragma once

#include <cstddef>

namespace cch::tui {

class Tui;

} // namespace cch::tui

namespace cch::tui::detail::testing {

/// Return the number of frame-level line preparations performed by the last
/// Tui render (composed buffer rows plus dock rows).
///
/// This private hook exists only for the frame preparation regression test: a
/// Preview Frame that changes nothing must prepare no rows, and a one-row view
/// change must prepare that row instead of the whole transcript.
[[nodiscard]] std::size_t frame_prepare_call_count(const cch::tui::Tui& tui) noexcept;

} // namespace cch::tui::detail::testing

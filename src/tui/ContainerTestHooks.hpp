#pragma once

#include <cstddef>

namespace cch::tui {

class Box;

} // namespace cch::tui

namespace cch::tui::detail::testing {

/// Return the number of line tokenizations performed by the last Box render.
/// This private hook exists only for the Box cache regression test.
[[nodiscard]] std::size_t box_tokenize_terminal_output_call_count(const cch::tui::Box& box) noexcept;

} // namespace cch::tui::detail::testing

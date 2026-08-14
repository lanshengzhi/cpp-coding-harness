#pragma once

#include <cch/support/Error.hpp>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace cch::tui {

/// Visible terminal width of `text` in columns. Tabs count as 3 columns; ANSI,
/// OSC 8 hyperlinks, and combining marks contribute nothing; the widest
/// physical line determines the result when newlines are present.
[[nodiscard]] std::size_t visible_width(std::string_view text);

/// Word-wrap `text` to at most `width` visible columns per line, preserving
/// ANSI styling across physical line breaks (pi `wrapTextWithAnsi`).
[[nodiscard]] support::Expected<std::vector<std::string>> wrap_text(
    std::string_view text,
    std::size_t width);

/// Truncate `text` to at most `max_width` visible columns, appending
/// `ellipsis` when truncation occurs and optionally padding to exactly
/// `max_width` (pi `truncateToWidth`).
[[nodiscard]] support::Expected<std::string> truncate_text(
    std::string_view text,
    std::size_t max_width,
    std::string_view ellipsis = "...",
    bool pad = false);

/// Extract the text occupying visible columns `[start_col, start_col + length)`
/// of `line`, carrying ANSI styling into the result. With `strict`, a wide
/// grapheme extending past the range end is excluded (pi `sliceByColumn`).
[[nodiscard]] support::Expected<std::string> slice_by_column(
    std::string_view line,
    std::size_t start_col,
    std::size_t length,
    bool strict = false);

/// Remove ANSI, OSC, and APC control sequences from `text`, preserving the
/// visible text (pi `stripTerminalSequences`).
[[nodiscard]] std::string strip_terminal_sequences(std::string_view text);

} // namespace cch::tui

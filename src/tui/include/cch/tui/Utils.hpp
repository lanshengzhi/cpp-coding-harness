#pragma once

#include <cch/support/Error.hpp>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace cch::tui {

/// Visible terminal width of `text` in columns. Tabs count as 3 columns; ANSI,
/// OSC 8 hyperlinks, and combining marks contribute nothing; newlines contribute
/// nothing and do not reset the accumulator (frozen pi `visibleWidth` sums every
/// grapheme across logical lines).
[[nodiscard]] std::size_t visible_width(std::string_view text);

/// Word-wrap `text` to at most `width` visible columns per line, preserving
/// ANSI styling across physical line breaks (pi `wrapTextWithAnsi`).
[[nodiscard]] support::Expected<std::vector<std::string>> wrap_text(std::string_view text, std::size_t width);

/// Truncate `text` to at most `max_width` visible columns, appending
/// `ellipsis` when truncation occurs and optionally padding to exactly
/// `max_width` (pi `truncateToWidth`).
[[nodiscard]] support::Expected<std::string> truncate_text(
        std::string_view text, std::size_t max_width, std::string_view ellipsis = "...", bool pad = false);

/// Extract the text occupying visible columns `[start_col, start_col + length)`
/// of `line`, carrying ANSI styling into the result. With `strict`, a wide
/// grapheme extending past the range end is excluded (pi `sliceByColumn`).
[[nodiscard]] support::Expected<std::string> slice_by_column(
        std::string_view line, std::size_t start_col, std::size_t length, bool strict = false);

/// Remove ANSI, OSC, and APC control sequences from `text`, preserving the
/// visible text (pi `stripTerminalSequences`).
[[nodiscard]] std::string strip_terminal_sequences(std::string_view text);

/// Text already present immediately before and after a paste insertion point.
/// Only the character nearest the insertion point is inspected, so callers may
/// pass any suffix of the preceding line and any prefix of the following one.
struct PasteBoundaries {
    std::string_view text_before{};
    std::string_view text_after{};
};

/// Passive options describing the insertion target of a paste (pi's
/// `Editor`/`Input` `handlePaste` differ exactly in `multiline`).
struct PasteNormalization {
    bool multiline{true};
    PasteBoundaries boundaries{};
};

/// Normalize pasted content before it is inserted, as frozen pi does:
///
/// - CR, CRLF and lone LF all collapse to one LF, kept in a multiline target
///   and dropped in a single-line one;
/// - TAB expands to four columns in both targets;
/// - a complete Kitty `CSI u` control reply inside the paste resolves to the
///   character it encodes (`ESC [ 106 ; 5 u` is Ctrl+J, a newline) instead of
///   leaking its printable parameter text;
/// - remaining C0, C1 and DEL control bytes are filtered out;
/// - a pasted file path that directly follows a word character (in the text
///   before it, or earlier in the same paste) gains one separating space, and
///   the same applies to the path's end against following text.
///
/// The result is ordinary UTF-8 text suitable for idiomatic C++ storage.
[[nodiscard]] std::string normalize_pasted_text(
        std::string_view text, PasteNormalization normalization = PasteNormalization{});

} // namespace cch::tui

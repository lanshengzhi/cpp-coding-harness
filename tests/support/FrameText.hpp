#pragma once

// Oracles for text that is wider than the frame it renders into (issue #1232
// adjacent finding, recorded in
// docs/research/pi-v1.3.0/1232-root-disk-timing-diagnostics-2026-11-11.md).
//
// Two product rules apply to a workspace or session path that is wider than the
// frame, and a screen assertion must name the one its component uses:
//
//  * a single-row consumer truncates. The footer's pwd row is
//    pi `truncateToWidth(pwd, width, "...")`: the leading cells survive and the
//    tail is replaced by the ellipsis (`coding_agent/tui/Footer.cpp`).
//  * a wrapping `Text` breaks the line. A dialog title, a status message, and
//    the editor wrap the same path into consecutive rows at the frame width
//    (`cch::tui::Text` -> `cch::tui::wrap_text`).
//
// The assertions built on these oracles read what a user actually sees, so they
// keep their discriminating form when the temporary root grows: the old
// `screen.find(workspace_path)` shape held only while the path fit one row. The
// rules themselves stay pinned with literal expectations --
// `Footer truncates the pwd and right side to the width` and
// `Footer renders a pwd that fits the width unchanged` (footer) and
// `wrap_text pushes a whitespace-only prefix before a long token` (wrap). The
// oracles are spelled here rather than called from `cch::tui` on purpose: a
// product change to either rule (a middle ellipsis, a `~` shorthand, no
// ellipsis, a different break) then fails the screen case instead of travelling
// with it.
//
// Every entry point assumes an ASCII text (an absolute path or a path
// fragment), so cell width and byte length coincide.

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace cch::tests {

/// The cells pi's single-row truncation keeps: everything when `text` fits
/// `width`, otherwise the leading `width - 3` cells that leave room for `...`.
[[nodiscard]] inline std::string truncated_row_prefix(std::string_view text, std::size_t width) {
    if (text.size() <= width) return std::string{text};
    return std::string{text.substr(0, width >= 3 ? width - 3 : 0)};
}

/// pi's `truncateToWidth(text, width, "...")` for an ASCII text: the whole text
/// when it fits, otherwise the retained prefix plus the ellipsis, filling
/// exactly `width` cells.
[[nodiscard]] inline std::string truncated_row(std::string_view text, std::size_t width) {
    if (text.size() <= width) return std::string{text};
    return truncated_row_prefix(text, width) + "...";
}

/// The consecutive row segments a wrapping component renders for an ASCII text
/// wider than `width`: `wrap_text`'s hard break, at the frame width, for a text
/// with no break opportunity. Asserting every segment on a raw byte stream (a
/// real PTY case, where the composed rows are not available) keeps "the whole
/// text is displayed" -- a truncating renderer would drop the tail.
[[nodiscard]] inline std::vector<std::string> wrapped_rows(std::string_view text, std::size_t width) {
    if (width == 0) return {std::string{text}};
    std::vector<std::string> rows;
    for (std::size_t offset = 0; offset < text.size(); offset += width) {
        rows.emplace_back(text.substr(offset, width));
    }
    if (rows.empty()) rows.emplace_back();
    return rows;
}

/// True when `text` is on the screen `rows` as a user reads it, allowing the
/// row breaks the renderer inserted: some run of consecutive rows, with each
/// row's trailing padding dropped, tiles `text` exactly. The match may start
/// anywhere in a row (a status line puts it after its label) and crosses a row
/// break only from a row that filled the frame -- that is the only place
/// `wrap_text` breaks a text -- skipping the next row's leading indent, the
/// `Text` padding that repeats on every wrapped line.
///
/// This is the wrapped form of `screen.find(text)` and it keeps the same
/// discriminating power for a path: a renderer that truncates the tail, drops a
/// segment, or shortens the middle leaves `text` untileable. A short row cannot
/// be crossed, so a needle is never assembled from two unrelated rows.
[[nodiscard]] inline bool rows_show_text(
        const std::vector<std::string>& rows, std::size_t width, std::string_view text) {
    if (text.empty()) return true;
    const auto without_padding = [](std::string_view row) {
        while (!row.empty() && row.back() == ' ')
            row.remove_suffix(1);
        return row;
    };
    // A row that filled the frame keeps at most the component's one-column
    // left indent beside its text (`Text` renders padding_x on every wrapped
    // line, so a full content row can end one cell short of the frame).
    const auto filled_frame = [width, &without_padding](
                                      std::string_view row) { return without_padding(row).size() + 1 >= width; };
    for (std::size_t start_row = 0; start_row < rows.size(); ++start_row) {
        const auto first = without_padding(rows[start_row]);
        for (std::size_t start_column = 0; start_column < first.size(); ++start_column) {
            std::size_t matched = 0;
            std::size_t row = start_row;
            std::size_t column = start_column;
            while (true) {
                const auto line = without_padding(rows[row]);
                while (column < line.size() && matched < text.size() && line[column] == text[matched]) {
                    ++column;
                    ++matched;
                }
                if (matched == text.size()) return true;
                if (column < line.size()) break; // the row diverged from `text`
                if (row + 1 >= rows.size()) break;
                if (!filled_frame(rows[row])) break; // a short row does not continue the text
                ++row;
                column = 0;
                while (column < rows[row].size() && rows[row][column] == ' ')
                    ++column; // the wrapped line's indent
            }
        }
    }
    return false;
}

} // namespace cch::tests

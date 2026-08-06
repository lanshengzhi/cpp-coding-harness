#pragma once

#include "tui/UnicodeWidth.hpp"

#include <utf8proc.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace cch::tui::detail {

// Behavioral baseline: pi 83114817 packages/tui/src/word-navigation.ts
// (findWordBackward/findWordForward), ported over per-grapheme units.

/// Grapheme word classification shared by the Editor and the single-line
/// Input: ASCII alphanumerics plus underscore, and the Unicode letter/number
/// categories. This is the C++ per-grapheme port of pi's Intl.Segmenter
/// word-like segments (divergence recorded in parity map #2: pi groups a
/// word-like run as one segment and skips to its punctuation boundary, while
/// the C++ fork classifies each grapheme).
[[nodiscard]] inline bool is_word_segment(std::string_view segment) {
    const auto [codepoint, bytes] = decode_utf8(segment, 0);
    if (bytes == 0) return false;
    if (codepoint < 128) {
        return (codepoint >= 'a' && codepoint <= 'z') || (codepoint >= 'A' && codepoint <= 'Z') ||
            (codepoint >= '0' && codepoint <= '9') || codepoint == '_';
    }
    const auto category = utf8proc_category(static_cast<utf8proc_int32_t>(codepoint));
    return category == UTF8PROC_CATEGORY_LU || category == UTF8PROC_CATEGORY_LL ||
        category == UTF8PROC_CATEGORY_LT || category == UTF8PROC_CATEGORY_LM ||
        category == UTF8PROC_CATEGORY_LO || category == UTF8PROC_CATEGORY_ND ||
        category == UTF8PROC_CATEGORY_NL || category == UTF8PROC_CATEGORY_NO;
}

/// Find the cursor position after moving one word backward from `cursor` in a
/// single line of graphemes (pi `findWordBackward`, per-grapheme port of the
/// Editor's `move_word`): skip the non-word run, then one word run.
[[nodiscard]] inline std::size_t find_word_backward(
    const std::vector<std::string>& graphemes,
    std::size_t cursor) {
    while (cursor > 0 && !is_word_segment(graphemes[cursor - 1])) --cursor;
    while (cursor > 0 && is_word_segment(graphemes[cursor - 1])) --cursor;
    return cursor;
}

/// Find the cursor position after moving one word forward from `cursor` in a
/// single line of graphemes (pi `findWordForward`).
[[nodiscard]] inline std::size_t find_word_forward(
    const std::vector<std::string>& graphemes,
    std::size_t cursor) {
    while (cursor < graphemes.size() && !is_word_segment(graphemes[cursor])) ++cursor;
    while (cursor < graphemes.size() && is_word_segment(graphemes[cursor])) ++cursor;
    return cursor;
}

} // namespace cch::tui::detail

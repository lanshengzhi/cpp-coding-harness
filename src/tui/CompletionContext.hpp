#pragma once

#include "tui/CjkBreakRanges.hpp"
#include "tui/UnicodeWidth.hpp"

#include <utf8proc.h>

#include <cstddef>
#include <string_view>
#include <vector>

namespace cch::tui::detail {

// Behavioral baseline: pi v1.0.4 at 7c10bd4337495ee613f2224843ecdf349b80d1df,
// `packages/tui/src/utils.ts` (autocompleteSeparatorRegex,
// autocompleteBoundaryRegex, cjkBreakRegex, cjkPunctuationRegex) and
// `packages/tui/src/autocomplete.ts` (PATH_DELIMITERS, PATH_WRAPPERS,
// stripLeadingWrappers, findLastDelimiter, isTokenStart) with
// `packages/tui/src/components/editor.ts` (buildTriggerPattern,
// buildDebouncePattern, unquotedAutocompleteSuffixRegex).
//
// This header is the single completion-context authority: the Editor's trigger
// classification and the provider's prefix extraction classify tokens with the
// same separator, wrapper, and boundary rules. Frozen observations live in
// `fixtures/pi-tui/bundles/pi-v1.0.4/autocomplete.json` and
// `editor-autocomplete.json` and are replayed by
// `tests/tui/AutocompleteContextTest.cpp`.

/// Byte offset of the code point ending at `index` (which must be a code-point
/// boundary inside `text`).
[[nodiscard]] inline std::size_t codepoint_start_before(std::string_view text, std::size_t index) {
    if (index == 0) return 0;
    std::size_t start = index - 1;
    while (start > 0 && (static_cast<unsigned char>(text[start]) & 0xC0) == 0x80)
        --start;
    return start;
}

/// The code points JavaScript's `\s` matches (ECMAScript WhiteSpace plus
/// LineTerminator). pi trims completion text and matches tokens with this set.
[[nodiscard]] inline bool is_completion_whitespace(char32_t codepoint) {
    switch (codepoint) {
    case U'\t':
    case U'\n':
    case U'\v':
    case U'\f':
    case U'\r':
    case U' ':
    case 0x00A0:
    case 0x1680:
    case 0x2028:
    case 0x2029:
    case 0x202F:
    case 0x205F:
    case 0x3000:
    case 0xFEFF:
        return true;
    default:
        return codepoint >= 0x2000 && codepoint <= 0x200A;
    }
}

/// The CJK punctuation pi names literally in `cjkPunctuationRegex`; the rest of
/// that set is "CJK code point that is also Unicode punctuation".
[[nodiscard]] inline bool is_named_cjk_punctuation(char32_t codepoint) {
    switch (codepoint) {
    case 0xFF0C: // ，
    case 0xFF0E: // ．
    case 0xFF1A: // ：
    case 0xFF1B: // ；
    case 0xFF01: // ！
    case 0xFF1F: // ？
    case 0xFF08: // （
    case 0xFF09: // ）
    case 0xFF3B: // ［
    case 0xFF3D: // ］
    case 0xFF5B: // ｛
    case 0xFF5D: // ｝
    case 0x201C: // “
    case 0x201D: // ”
    case 0x2018: // ‘
    case 0x2019: // ’
    case 0x2026: // …
    case 0x2014: // —
        return true;
    default:
        return false;
    }
}

/// pi's `(?=\p{Punctuation})cjkBreakRegex` alternative: a CJK code point that
/// Unicode also classifies as punctuation.
[[nodiscard]] inline bool is_cjk_punctuation_codepoint(char32_t codepoint) {
    if (!is_cjk_break_codepoint(codepoint)) return false;
    switch (utf8proc_category(static_cast<utf8proc_int32_t>(codepoint))) {
    case UTF8PROC_CATEGORY_PC:
    case UTF8PROC_CATEGORY_PD:
    case UTF8PROC_CATEGORY_PS:
    case UTF8PROC_CATEGORY_PE:
    case UTF8PROC_CATEGORY_PI:
    case UTF8PROC_CATEGORY_PF:
    case UTF8PROC_CATEGORY_PO:
        return true;
    default:
        return false;
    }
}

/// pi's `autocompleteSeparatorRegex`: whitespace or CJK punctuation. Only these
/// code points end a completion token; CJK letters never do.
[[nodiscard]] inline bool is_completion_separator(char32_t codepoint) {
    return is_completion_whitespace(codepoint) || is_named_cjk_punctuation(codepoint) ||
           is_cjk_punctuation_codepoint(codepoint);
}

/// pi's `PATH_DELIMITERS` (" ", "\t", '"', '\'', "="). These separate tokens
/// for prefix extraction without being separators for trigger patterns.
[[nodiscard]] inline bool is_completion_path_delimiter(char32_t codepoint) {
    return codepoint == U' ' || codepoint == U'\t' || codepoint == U'"' || codepoint == U'\'' || codepoint == U'=';
}

/// Opening prose wrappers pi accepts before a token (`PATH_WRAPPERS`).
[[nodiscard]] inline bool is_completion_wrapper(char character) {
    return character == '(' || character == '[' || character == '{' || character == '<' || character == '`';
}

struct CompletionDelimiter {
    std::size_t offset{std::string_view::npos};
    std::size_t length{0};

    [[nodiscard]] std::size_t token_start() const noexcept {
        return offset == std::string_view::npos ? 0 : offset + length;
    }
};

/// The last path delimiter or separator code point, or an empty match.
/// pi's `findLastDelimiter` iterates code points, so a token keeps every
/// non-separator code point, including the CJK letters inside a path.
[[nodiscard]] inline CompletionDelimiter find_last_completion_delimiter(std::string_view text) {
    CompletionDelimiter last{};
    std::size_t index = 0;
    while (index < text.size()) {
        const auto [codepoint, length] = decode_utf8(text, index);
        if (is_completion_path_delimiter(codepoint) || is_completion_separator(codepoint)) {
            last = {.offset = index, .length = length};
        }
        index += length;
    }
    return last;
}

/// Whether any code point of `text` ends a completion token.
[[nodiscard]] inline bool contains_completion_separator(std::string_view text) {
    std::size_t index = 0;
    while (index < text.size()) {
        const auto [codepoint, length] = decode_utf8(text, index);
        if (is_completion_separator(codepoint)) return true;
        index += length;
    }
    return false;
}

/// Whether `text` ends at a token boundary: empty, or ending with a separator
/// (pi's `autocompleteBoundaryRegex` anchored at the end).
[[nodiscard]] inline bool ends_at_completion_boundary(std::string_view text) {
    if (text.empty()) return true;
    return is_completion_separator(decode_utf8(text, codepoint_start_before(text, text.size())).first);
}

/// The last code point of `text`, or U+0 when it is empty.
[[nodiscard]] inline char32_t last_codepoint_of(std::string_view text) {
    if (text.empty()) return 0;
    return decode_utf8(text, codepoint_start_before(text, text.size())).first;
}

/// pi's `isTokenStart`: `index` starts a token when the prose wrappers before it
/// are preceded by nothing, a path delimiter, or a separator.
[[nodiscard]] inline bool is_completion_token_start(std::string_view text, std::size_t index) {
    std::size_t start = index;
    while (start > 0 && is_completion_wrapper(text[start - 1]))
        --start;
    if (start == 0) return true;
    if (is_completion_path_delimiter(static_cast<char32_t>(static_cast<unsigned char>(text[start - 1])))) {
        return true;
    }
    return ends_at_completion_boundary(text.substr(0, start));
}

/// pi's `stripLeadingWrappers`: drop unmatched opening wrappers and keep a
/// wrapper whose closer appears inside the token.
[[nodiscard]] inline std::string_view strip_leading_completion_wrappers(std::string_view token) {
    while (!token.empty()) {
        char closer = '\0';
        switch (token.front()) {
        case '(':
            closer = ')';
            break;
        case '[':
            closer = ']';
            break;
        case '{':
            closer = '}';
            break;
        case '<':
            closer = '>';
            break;
        case '`':
            closer = '`';
            break;
        default:
            return token;
        }
        if (token.find(closer, 1) != std::string_view::npos) break;
        token.remove_prefix(1);
    }
    return token;
}

/// pi's `String.prototype.trimStart` over the text before the cursor.
[[nodiscard]] inline std::string_view trim_start_completion_whitespace(std::string_view text) {
    std::size_t index = 0;
    while (index < text.size()) {
        const auto [codepoint, length] = decode_utf8(text, index);
        if (!is_completion_whitespace(codepoint)) break;
        index += length;
    }
    return text.substr(index);
}

/// pi's `String.prototype.trim` over the text before the cursor.
[[nodiscard]] inline std::string_view trim_completion_whitespace(std::string_view text) {
    const auto leading = trim_start_completion_whitespace(text);
    std::size_t end = leading.size();
    while (end > 0 && is_completion_whitespace(decode_utf8(leading, codepoint_start_before(leading, end)).first)) {
        end = codepoint_start_before(leading, end);
    }
    return leading.substr(0, end);
}

/// pi's word characters for retriggering completion while typing: ASCII
/// alphanumerics, `.`, `-`, `_`, or a CJK code point (pi's
/// `/[a-zA-Z0-9.\-_]/ || cjkBreakRegex` in `Editor.insertCharacter`).
[[nodiscard]] inline bool is_completion_word_codepoint(char32_t codepoint) {
    if ((codepoint >= U'a' && codepoint <= U'z') || (codepoint >= U'A' && codepoint <= U'Z') ||
            (codepoint >= U'0' && codepoint <= U'9')) {
        return true;
    }
    if (codepoint == U'.' || codepoint == U'-' || codepoint == U'_') return true;
    return is_cjk_break_codepoint(codepoint);
}

/// pi's `buildTriggerPattern` / `buildDebouncePattern`: whether the text before
/// the cursor ends in a trigger token that starts at a token boundary, allowing
/// prose wrappers and a quoted attachment token (pi's
/// `autocompleteTokenStartSource` plus `unquotedAutocompleteSuffixRegex`).
[[nodiscard]] inline bool matches_completion_trigger(
        std::string_view text_before_cursor, const std::vector<std::string>& trigger_characters) {
    const auto token_matches = [&](std::size_t start) {
        std::size_t index = start;
        while (index < text_before_cursor.size() && is_completion_wrapper(text_before_cursor[index]))
            ++index;
        for (const auto& trigger : trigger_characters) {
            if (trigger.empty() || text_before_cursor.compare(index, trigger.size(), trigger) != 0) continue;
            // A quoted attachment token may contain separators; an unquoted
            // token may not.
            if (trigger == "@" && index + 1 < text_before_cursor.size() && text_before_cursor[index + 1] == '"' &&
                    text_before_cursor.find('"', index + 2) == std::string_view::npos) {
                return true;
            }
            if (!contains_completion_separator(text_before_cursor.substr(index + trigger.size()))) return true;
        }
        return false;
    };

    std::size_t index = 0;
    while (true) {
        const bool at_boundary =
                index == 0 ||
                is_completion_separator(
                        decode_utf8(text_before_cursor, codepoint_start_before(text_before_cursor, index)).first);
        if (at_boundary && token_matches(index)) return true;
        if (index >= text_before_cursor.size()) return false;
        index += decode_utf8(text_before_cursor, index).second;
    }
}

} // namespace cch::tui::detail

#include <cch/tui/Utils.hpp>

#include "tui/UnicodeWidth.hpp"

#include <cch/support/Error.hpp>
#include <utf8proc.h>

#include <algorithm>
#include <cstddef>
#include <format>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::tui {
namespace {

/// The ANSI SGR reset pi's truncateToWidth always emits around the ellipsis.
constexpr std::string_view kSgrReset{"\x1b[0m"};

/// One closed code-point range of pi's CJK line-break set.
struct CjkBreakRange {
    char32_t first{0};
    char32_t last{0};
};

/// pi's CJK line-break code points as generated range data: utf8proc exposes
/// no Script_Extensions property, so the set cannot be re-derived here. The
/// generated include carries pi's regex text, the pinned revision, and its
/// generator command.
constexpr CjkBreakRange kCjkBreakRanges[]{
#define CCH_CJK_BREAK_RANGE(first, last) {first, last},
#include "tui/CjkBreakRanges.inc"
#undef CCH_CJK_BREAK_RANGE
};

/// True when the code point is in pi's CJK break set. The generated ranges are
/// sorted and non-overlapping, so the first one ending at or after the code
/// point is the only candidate.
[[nodiscard]] bool is_cjk_break_codepoint(char32_t codepoint) {
    const auto range = std::ranges::lower_bound(kCjkBreakRanges, codepoint, {}, &CjkBreakRange::last);
    return range != std::end(kCjkBreakRanges) && codepoint >= range->first;
}

/// True when the code point is whitespace: the separator class the wrap path
/// breaks on and the trailing class pi's `wrapSingleLine` removes with
/// `line.trimEnd()`.
[[nodiscard]] bool is_whitespace_codepoint(char32_t codepoint) {
    if (codepoint == ' ') return true;
    const auto category = utf8proc_category(static_cast<utf8proc_int32_t>(codepoint));
    return category == UTF8PROC_CATEGORY_ZS || category == UTF8PROC_CATEGORY_ZL || category == UTF8PROC_CATEGORY_ZP;
}

/// pi's `String.prototype.trimEnd` over a wrapped line: trailing whitespace is
/// removed only when nothing was appended after it, so a line-end reset keeps
/// the whitespace that precedes it (`wrapSingleLine`'s final `line.trimEnd()`).
void trim_end_whitespace(std::string& text) {
    std::size_t end = text.size();
    while (end > 0) {
        std::size_t start = end - 1;
        while (start > 0 && (static_cast<unsigned char>(text[start]) & 0xC0) == 0x80)
            --start;
        const auto [codepoint, bytes] = detail::decode_utf8(std::string_view(text).substr(start, end - start), 0);
        if (bytes == 0 || !is_whitespace_codepoint(codepoint)) break;
        end = start;
    }
    text.resize(end);
}

/// True when any code point of the grapheme cluster is in pi's CJK break set.
/// pi tests the whole grapheme segment, so a base carrying such a combining
/// mark is a break opportunity and a cluster is never broken apart.
[[nodiscard]] bool is_cjk_break_cluster(std::string_view cluster) {
    std::size_t position = 0;
    while (position < cluster.size()) {
        const auto [codepoint, bytes] = detail::decode_utf8(cluster, position);
        if (bytes == 0) break;
        if (is_cjk_break_codepoint(codepoint)) return true;
        position += bytes;
    }
    return false;
}

} // namespace

std::size_t visible_width(std::string_view text) {
    auto tokens = detail::tokenize_terminal_output(text);
    if (!tokens) return 0;
    std::size_t maximum = 0;
    std::size_t current = 0;
    for (const auto& token : *tokens) {
        if (token.kind == detail::TerminalTokenKind::Newline) {
            maximum = std::max(maximum, current);
            current = 0;
        } else {
            current += token.width;
        }
    }
    return std::max(maximum, current);
}

support::Expected<std::vector<std::string>> wrap_text(std::string_view text, std::size_t width) {
    if (width == 0) {
        return std::unexpected(detail::invalid_terminal_text("Wrap width must be positive"));
    }
    auto tokens = detail::tokenize_terminal_output(text);
    if (!tokens) return std::unexpected(tokens.error());

    const auto is_whitespace = [](const detail::TerminalToken& token) {
        if (token.kind != detail::TerminalTokenKind::Grapheme) return false;
        const auto [codepoint, bytes] = detail::decode_utf8(token.text, 0);
        return bytes != 0 && is_whitespace_codepoint(codepoint);
    };
    const auto is_cjk_break_token = [](const detail::TerminalToken& token) {
        return token.kind == detail::TerminalTokenKind::Grapheme && is_cjk_break_cluster(token.text);
    };

    std::vector<std::string> lines;
    std::vector<detail::TerminalToken> pending_separator;
    detail::AnsiStyleState style;
    std::string line;
    std::size_t line_width = 0;
    std::size_t pending_width = 0;
    std::size_t input_line_start = 0;
    bool input_line_wrapped = false;

    const auto clear_pending = [&]() {
        pending_separator.clear();
        pending_width = 0;
    };
    const auto append_token = [&](const detail::TerminalToken& token) {
        line += token.text;
        if (token.kind == detail::TerminalTokenKind::Grapheme) {
            line_width += token.width;
        } else {
            style.process_ansi(token.text);
        }
    };
    // A wrap break pushes the current line and starts the next one from the
    // codes that are still active. pi closes underline/hyperlink only at such a
    // boundary (`AnsiCodeTracker.getLineEndReset`); the full reset belongs to
    // the composed row. A caller that breaks on a word boundary trims the line
    // first, the other two break sites push it as-is.
    const auto push_wrapped_line = [&]() {
        line += style.get_line_end_reset();
        lines.push_back(std::move(line));
        line = style.get_active_codes();
        line_width = 0;
        input_line_wrapped = true;
    };
    const auto replay_pending = [&](bool keep_whitespace) {
        for (const auto& pending : pending_separator) {
            if (keep_whitespace || pending.kind != detail::TerminalTokenKind::Grapheme) {
                append_token(pending);
            }
        }
        clear_pending();
    };
    // pi's tokenizer attaches an escape sequence to the next visible grapheme,
    // so controls staged after the last whitespace belong to the upcoming token
    // rather than to the line being pushed (`splitIntoTokensWithAnsi`).
    const auto replay_pending_prefix = [&]() {
        std::size_t replayable = pending_separator.size();
        while (replayable > 0 && pending_separator[replayable - 1].kind != detail::TerminalTokenKind::Grapheme) {
            --replayable;
        }
        for (std::size_t position = 0; position < replayable; ++position) {
            append_token(pending_separator[position]);
        }
        pending_separator.erase(
                pending_separator.begin(), pending_separator.begin() + static_cast<std::ptrdiff_t>(replayable));
        // The retained entries are zero-width controls.
        pending_width = 0;
    };
    // pi's `wrapSingleLine` runs its trailing `line.trimEnd()` over every line of
    // an input line that wrapped; an input line that fits keeps its trailing
    // whitespace. A logical newline is an input-line boundary, not a break.
    const auto finish_input_line = [&]() {
        if (input_line_wrapped) {
            for (std::size_t position = input_line_start; position < lines.size(); ++position) {
                trim_end_whitespace(lines[position]);
            }
        }
        input_line_start = lines.size();
        input_line_wrapped = false;
    };

    std::size_t index = 0;
    while (index < tokens->size()) {
        const auto& token = (*tokens)[index];
        if (token.kind == detail::TerminalTokenKind::Newline) {
            if (line_width + pending_width <= width) replay_pending(true);
            else replay_pending(false);
            // pi splits the input on `\r\n|\r|\n` and prefixes each line with
            // the previous line's active codes: a logical line boundary is not a
            // wrap break, so it carries no line-end reset.
            lines.push_back(std::move(line));
            line = style.get_active_codes();
            line_width = 0;
            finish_input_line();
            ++index;
            continue;
        }
        if (is_whitespace(token)) {
            pending_separator.push_back(token);
            pending_width += token.width;
            ++index;
            continue;
        }
        if (token.kind != detail::TerminalTokenKind::Grapheme) {
            if (pending_separator.empty()) append_token(token);
            else pending_separator.push_back(token);
            ++index;
            continue;
        }

        // pi splits every grapheme matching its CJK break set into a token of
        // its own (`splitIntoTokensWithAnsi`, utils.ts at the frozen baseline),
        // so such a grapheme is a break opportunity by itself; every other
        // token runs to the next whitespace, CJK grapheme, or newline.
        auto word_end = index;
        std::size_t word_width = 0;
        if (is_cjk_break_token(token)) {
            word_width = token.width;
            ++word_end;
        } else {
            while (word_end < tokens->size()) {
                const auto& word_token = (*tokens)[word_end];
                if (word_token.kind == detail::TerminalTokenKind::Newline || is_whitespace(word_token) ||
                        is_cjk_break_token(word_token)) {
                    break;
                }
                word_width += word_token.width;
                ++word_end;
            }
        }

        if (word_width <= width) {
            if (line_width + pending_width + word_width > width) {
                if (line_width != 0) {
                    replay_pending_prefix();
                    // pi trims the line it breaks at, then appends the reset.
                    trim_end_whitespace(line);
                    push_wrapped_line();
                }
                replay_pending(false);
            } else {
                replay_pending(true);
            }
            while (index < word_end) append_token((*tokens)[index++]);
            continue;
        }

        // A token longer than the width starts on a fresh line: pi pushes the
        // current line whenever it holds anything at all (`if (currentLine)`,
        // which a whitespace-only prefix satisfies) and chunks the token at
        // exactly the width from the new line (`wrapSingleLine` and
        // `breakLongWord`, utils.ts at the frozen baseline); it never fills the
        // remainder of the current line.
        if (line_width != 0 || !pending_separator.empty()) {
            replay_pending_prefix();
            push_wrapped_line();
        }
        replay_pending(false);

        while (index < word_end) {
            const auto& word_token = (*tokens)[index++];
            if (word_token.kind != detail::TerminalTokenKind::Grapheme) {
                append_token(word_token);
                continue;
            }
            if (word_token.width > width) {
                return std::unexpected(detail::invalid_terminal_text(
                    "Unicode grapheme is wider than the available terminal width",
                    std::format(
                        "grapheme width {} exceeds visible width {}",
                        word_token.width,
                        width)));
            }
            if (line_width != 0 && line_width + word_token.width > width) push_wrapped_line();
            append_token(word_token);
        }
    }

    if (line_width + pending_width <= width) replay_pending(true);
    else replay_pending(false);
    // pi `wrapSingleLine` appends the line-end reset only where it breaks a
    // line: "No reset at end of final line - let caller handle it". The full
    // reset for the row belongs to the composed-line boundary.
    lines.push_back(std::move(line));
    finish_input_line();
    return lines;
}

support::Expected<std::string> truncate_text(
    std::string_view text,
    std::size_t max_width,
    std::string_view ellipsis,
    bool pad) {
    if (max_width == 0) return std::string{};

    auto tokens = detail::tokenize_terminal_output(text);
    if (!tokens) return std::unexpected(tokens.error());
    auto width_result = detail::token_width(*tokens);
    if (!width_result) return std::unexpected(width_result.error());

    if (*width_result <= max_width) {
        detail::AnsiStyleState style;
        auto result = detail::normalized_text(*tokens);
        for (const auto& token : *tokens) {
            if (token.kind != detail::TerminalTokenKind::Grapheme) style.process_ansi(token.text);
        }
        // debt: this fits path closes underline/hyperlink before padding, where
        // pi pads inside the still-open span (`truncateToWidth("\x1b[4mabc", 8,
        // "", true)` is "\x1b[4mabc     " in pi and
        // "\x1b[4mabc\x1b[24m     " here); upgrade when a composed surface
        // pads a line carrying an open underline/hyperlink.
        result += style.get_line_end_reset();
        if (pad) result.append(max_width - *width_result, ' ');
        return result;
    }

    auto ellipsis_tokens = detail::tokenize_terminal_output(ellipsis);
    if (!ellipsis_tokens) return std::unexpected(ellipsis_tokens.error());
    for (const auto& token : *ellipsis_tokens) {
        if (token.kind != detail::TerminalTokenKind::Grapheme) {
            return std::unexpected(
                detail::invalid_terminal_text("Truncation ellipsis must contain only visible text"));
        }
    }
    const auto ellipsis_width = visible_width(ellipsis);
    if (ellipsis_width > max_width) return truncate_text(ellipsis, max_width, "", pad);
    const auto target_width = max_width - ellipsis_width;

    std::string result;
    std::size_t collected_width = 0;
    for (const auto& token : *tokens) {
        if (token.kind != detail::TerminalTokenKind::Grapheme) {
            result += token.text;
            continue;
        }
        if (collected_width + token.width > target_width) break;
        result += token.text;
        collected_width += token.width;
    }
    // pi's `finalizeTruncatedResult` is exactly
    // `prefix + "\x1b[0m" + ellipsis + "\x1b[0m"` (utils.ts at the frozen
    // baseline); the always-on resets close whatever the kept prefix left open.
    result += kSgrReset;
    result += ellipsis;
    if (!ellipsis.empty()) result += kSgrReset;
    if (pad) result.append(max_width - collected_width - ellipsis_width, ' ');
    return result;
}

support::Expected<std::string> slice_by_column(
    std::string_view line,
    std::size_t start_col,
    std::size_t length,
    bool strict) {
    if (length == 0) return std::string{};
    const auto end_col = start_col + length;
    auto tokens = detail::tokenize_terminal_output(line);
    if (!tokens) return std::unexpected(tokens.error());

    std::string result;
    std::string pending_ansi;
    std::size_t current_col = 0;
    for (const auto& token : *tokens) {
        if (token.kind != detail::TerminalTokenKind::Grapheme) {
            if (token.kind == detail::TerminalTokenKind::Newline) {
                if (current_col >= start_col && current_col < end_col) result += token.text;
                continue;
            }
            if (current_col >= start_col && current_col < end_col) {
                result += token.text;
            } else if (current_col < start_col) {
                pending_ansi += token.text;
            }
            continue;
        }
        const auto in_range = current_col >= start_col && current_col < end_col;
        const auto fits = !strict || current_col + token.width <= end_col;
        if (in_range && fits) {
            if (!pending_ansi.empty()) {
                result += pending_ansi;
                pending_ansi.clear();
            }
            result += token.text;
        }
        current_col += token.width;
        if (current_col >= end_col) break;
    }
    return result;
}

std::string strip_terminal_sequences(std::string_view text) {
    if (text.find('\x1b') == std::string_view::npos) return std::string(text);

    std::string result;
    std::size_t position = 0;
    while (position < text.size()) {
        std::size_t end = std::string_view::npos;
        if (text[position] == '\x1b' && position + 1 < text.size()) {
            const auto kind = text[position + 1];
            if (kind == '[') {
                std::size_t cursor = position + 2;
                while (cursor < text.size() && text[cursor] != 'm' && text[cursor] != 'G' &&
                       text[cursor] != 'K' && text[cursor] != 'H' && text[cursor] != 'J') {
                    ++cursor;
                }
                if (cursor < text.size()) end = cursor + 1;
            } else if (kind == ']' || kind == '_') {
                std::size_t cursor = position + 2;
                while (cursor < text.size()) {
                    if (text[cursor] == '\x07') {
                        end = cursor + 1;
                        break;
                    }
                    if (text[cursor] == '\x1b' && cursor + 1 < text.size() && text[cursor + 1] == '\\') {
                        end = cursor + 2;
                        break;
                    }
                    ++cursor;
                }
            }
        }
        if (end == std::string_view::npos) {
            result += text[position];
            ++position;
        } else {
            position = end;
        }
    }
    return result;
}

} // namespace cch::tui

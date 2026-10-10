#include <cch/tui/Utils.hpp>

#include "tui/CjkBreakRanges.hpp"
#include "tui/UnicodeWidth.hpp"

#include <cch/support/Error.hpp>
#include <utf8proc.h>

#include <algorithm>
#include <cstddef>
#include <format>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::tui {
namespace {

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
        if (detail::is_cjk_break_codepoint(codepoint)) return true;
        position += bytes;
    }
    return false;
}

/// pi's `truncateFragmentToWidth` over an ellipsis already validated as visible
/// text: the leading graphemes that fit `max_width`.
[[nodiscard]] std::string clipped_visible_text(
        const std::vector<detail::TerminalToken>& tokens, std::size_t max_width) {
    std::string clipped;
    std::size_t width = 0;
    for (const auto& token : tokens) {
        if (width + token.width > max_width) break;
        clipped += token.text;
        width += token.width;
    }
    return clipped;
}

/// pi's `getActiveOsc8Close(prefix)`: the OSC 8 close matching the terminator of
/// the link the kept prefix leaves open, or nothing.
[[nodiscard]] std::string active_link_close(std::string_view prefix) {
    const auto tokens = detail::tokenize_terminal_output(prefix);
    if (!tokens) return {};
    detail::AnsiStyleState style;
    for (const auto& token : *tokens) {
        if (token.kind != detail::TerminalTokenKind::Grapheme) style.process_ansi(token.text);
    }
    return style.get_active_link_close();
}

/// pi's `finalizeTruncatedResult` (utils.ts at the frozen baseline): exactly
/// `prefix + osc8Close + "\x1b[0m" + ellipsis + "\x1b[0m"`, with the trailing
/// ellipsis reset present only when the ellipsis is non-empty and padding
/// appended afterwards. The always-on SGR reset closes what the kept prefix left
/// open, so no attribute is closed separately.
[[nodiscard]] std::string finalize_truncation(std::string prefix,
        std::size_t prefix_width,
        std::string_view ellipsis,
        std::size_t ellipsis_width,
        std::size_t max_width,
        bool pad) {
    const auto link_close = active_link_close(prefix);
    prefix += link_close;
    prefix += detail::kSgrReset;
    if (!ellipsis.empty()) {
        prefix += ellipsis;
        prefix += detail::kSgrReset;
    }
    if (pad) {
        const auto visible = prefix_width + ellipsis_width;
        if (max_width > visible) prefix.append(max_width - visible, ' ');
    }
    return prefix;
}

} // namespace

namespace detail {

VisibleWidthMeasurement measure_visible_width(std::string_view text) {
    const auto is_printable_ascii = [](unsigned char byte) { return byte >= 0x20 && byte <= 0x7E; };
    if (std::ranges::all_of(text, is_printable_ascii)) return {.width = text.size(), .used_tokenizer = false};

    // Match frozen pi visibleWidth: sum every grapheme/tab width. Newlines are
    // neither printable ASCII nor tokenizer width contributors; they do not
    // reset the accumulator to a widest-line reading.
    auto tokens = tokenize_terminal_output(text, TokenizeMode::WidthOnly);
    if (!tokens) return {.width = 0, .used_tokenizer = true};
    std::size_t width = 0;
    for (const auto& token : *tokens) {
        if (token.kind != TerminalTokenKind::Newline) width += token.width;
    }
    return {.width = width, .used_tokenizer = true};
}

} // namespace detail

std::size_t visible_width(std::string_view text) { return detail::measure_visible_width(text).width; }

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
    const auto push_line = [&]() {
        lines.push_back(std::move(line));
        line = style.get_active_codes();
        line_width = 0;
    };
    const auto push_wrapped_line = [&]() {
        line += style.get_line_end_reset();
        push_line();
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

    // True when the control at `index` is staged for the next visible grapheme
    // instead of closing the line being filled: pi attaches it to that grapheme
    // as its token prefix, so a wrap break between them leaves the pushed row
    // without it. A code followed by whitespace, a newline, or nothing keeps
    // its place on the current line.
    const auto pending_control_belongs_to_next_token = [&](std::size_t index) {
        if (line_width == 0) return false;
        for (std::size_t position = index + 1; position < tokens->size(); ++position) {
            const auto& next = (*tokens)[position];
            if (next.kind == detail::TerminalTokenKind::Newline) return false;
            if (next.kind != detail::TerminalTokenKind::Grapheme) continue;
            return !is_whitespace(next);
        }
        return false;
    };

    std::size_t index = 0;
    while (index < tokens->size()) {
        const auto& token = (*tokens)[index];
        if (token.kind == detail::TerminalTokenKind::Newline) {
            if (line_width + pending_width <= width)
                replay_pending(true);
            else
                replay_pending(false);
            // pi splits the input on `\r\n|\r|\n` and prefixes each line with
            // the previous line's active codes: a logical line boundary is not a
            // wrap break, so it carries no line-end reset.
            push_line();
            finish_input_line();
            ++index;
            continue;
        }
        // pi attaches ANSI controls to the following whitespace token. Such a
        // token is not `token.trim() === ""`: when over-long it must use the
        // same grapheme chunking as a word, rather than an unbounded separator
        // replay (#704). Inspect a whitespace run only once, before staging it.
        auto styled_space_end = index;
        std::size_t styled_space_width = 0;
        if (is_whitespace(token) && pending_separator.empty()) {
            bool has_control = index > 0 && (*tokens)[index - 1].kind != detail::TerminalTokenKind::Grapheme &&
                               (*tokens)[index - 1].kind != detail::TerminalTokenKind::Newline;
            has_control = has_control || (line_width == 0 && !line.empty());
            bool pending_control = false;
            auto position = index;
            while (position < tokens->size()) {
                const auto& candidate = (*tokens)[position];
                if (candidate.kind == detail::TerminalTokenKind::Newline) break;
                if (candidate.kind == detail::TerminalTokenKind::Grapheme) {
                    if (!is_whitespace(candidate)) break;
                    styled_space_width += candidate.width;
                    styled_space_end = position + 1;
                    has_control = has_control || pending_control;
                    pending_control = false;
                } else {
                    pending_control = true;
                }
                ++position;
            }
            if (!has_control || styled_space_width <= width) styled_space_end = index;
        }
        if (is_whitespace(token) && styled_space_end == index) {
            pending_separator.push_back(token);
            pending_width += token.width;
            ++index;
            continue;
        }
        if (token.kind != detail::TerminalTokenKind::Grapheme) {
            // pi's tokenizer holds an escape sequence in `pendingAnsi` until the
            // next visible grapheme attaches it, so the code belongs to the
            // upcoming token rather than to the line being filled. Deferring it
            // here keeps a long token's leading codes off the pushed row: pi
            // pushes the current line before `breakLongWord` sees them
            // (`wrapTextWithAnsi("\u4e2d\u6587\x1b[31mABCDEFGHIJ", 4)` is
            // ["\u4e2d\u6587", "\x1b[31mABCD", "\x1b[31mEFGH", "\x1b[31mIJ"]). A
            // code that no visible grapheme follows stays on the line, because
            // pi attaches a trailing code to the line's last token.
            if (!pending_separator.empty() || pending_control_belongs_to_next_token(index)) {
                pending_separator.push_back(token);
            } else {
                append_token(token);
            }
            ++index;
            continue;
        }

        // pi splits every grapheme matching its CJK break set into a token of
        // its own (`splitIntoTokensWithAnsi`, utils.ts at the frozen baseline),
        // so such a grapheme is a break opportunity by itself; every other
        // token runs to the next whitespace, CJK grapheme, or newline.
        auto word_end = index;
        std::size_t word_width = 0;
        if (styled_space_end != index) {
            word_width = styled_space_width;
            word_end = styled_space_end;
        } else if (is_cjk_break_token(token)) {
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
                // pi breaks here only when the line already carries visible
                // content (`currentVisibleLength > 0`), and the whitespace it
                // has already appended counts toward that, so a deferred
                // whitespace-only prefix still pushes a row.
                if (line_width != 0 || pending_width != 0) {
                    replay_pending_prefix();
                    // pi trims the line it breaks at, then appends the reset.
                    trim_end_whitespace(line);
                    push_wrapped_line();
                }
                replay_pending(false);
            } else {
                replay_pending(true);
            }
            while (index < word_end)
                append_token((*tokens)[index++]);
            continue;
        }

        // A token longer than the width starts on a fresh line: pi pushes the
        // current line whenever it holds anything at all (`if (currentLine)`,
        // which a whitespace-only prefix satisfies) and chunks the token at
        // exactly the width from the new line (`wrapSingleLine` and
        // `breakLongWord`, utils.ts at the frozen baseline); it never fills the
        // remainder of the current line.
        // debt: a zero-width control staged immediately after visible content
        // is emitted at the end of the pushed line, where pi stages it on the
        // continuation line (`wrap_text("中文\x1b[31mABCDEFGHIJ", 4)` here is
        // ["中文\x1b[31m", "\x1b[31mABCD", …] versus pi's ["中文",
        // "\x1b[31mABCD", …]); the rows render identically. Upgrade when a
        // composed surface observes a control's position within a row.
        if (line_width != 0 || !pending_separator.empty()) {
            replay_pending_prefix();
            // A pure whitespace separator may itself exceed the width. pi
            // drops its overflowing whitespace before the long-word push;
            // trim before the reset, which otherwise protects those spaces.
            if (line_width > width) trim_end_whitespace(line);
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
                return std::unexpected(
                        detail::invalid_terminal_text("Unicode grapheme is wider than the available terminal width",
                                std::format("grapheme width {} exceeds visible width {}", word_token.width, width)));
            }
            if (line_width != 0 && line_width + word_token.width > width) push_wrapped_line();
            append_token(word_token);
        }
    }

    if (line_width + pending_width <= width)
        replay_pending(true);
    else
        replay_pending(false);
    // pi `wrapSingleLine` appends the line-end reset only where it breaks a
    // line: "No reset at end of final line - let caller handle it". The full
    // reset for the row belongs to the composed-line boundary.
    lines.push_back(std::move(line));
    finish_input_line();
    return lines;
}

support::Expected<std::string> truncate_text(
        std::string_view text, std::size_t max_width, std::string_view ellipsis, bool pad) {
    if (max_width == 0) return std::string{};

    auto tokens = detail::tokenize_terminal_output(text);
    if (!tokens) return std::unexpected(tokens.error());
    auto width_result = detail::token_width(*tokens);
    if (!width_result) return std::unexpected(width_result.error());

    if (*width_result <= max_width) {
        // pi returns the fitting input unchanged and pads inside whatever span
        // it left open (`truncateToWidth`, utils.ts at the frozen baseline):
        // closing underline or the hyperlink before the padding would change the
        // bytes and let the padding fall outside the intended styling.
        auto result = detail::normalized_text(*tokens);
        if (pad) result.append(max_width - *width_result, ' ');
        return result;
    }

    auto ellipsis_tokens = detail::tokenize_terminal_output(ellipsis);
    if (!ellipsis_tokens) return std::unexpected(ellipsis_tokens.error());
    for (const auto& token : *ellipsis_tokens) {
        if (token.kind != detail::TerminalTokenKind::Grapheme) {
            return std::unexpected(detail::invalid_terminal_text("Truncation ellipsis must contain only visible text"));
        }
    }
    const auto ellipsis_width = visible_width(ellipsis);
    if (ellipsis_width >= max_width) {
        // pi clips the ellipsis itself when it does not leave room beside the
        // text: nothing visible survives, so the result is empty (or all
        // padding) rather than a reset around a missing ellipsis.
        auto clipped = clipped_visible_text(*ellipsis_tokens, max_width);
        const auto clipped_width = visible_width(clipped);
        if (clipped_width == 0) return pad ? std::string(max_width, ' ') : std::string{};
        return finalize_truncation({}, 0, clipped, clipped_width, max_width, pad);
    }
    const auto target_width = max_width - ellipsis_width;

    std::string result;
    std::string pending_ansi;
    std::size_t collected_width = 0;
    for (const auto& token : *tokens) {
        if (token.kind != detail::TerminalTokenKind::Grapheme) {
            // pi holds a control in `pendingAnsi` until a kept grapheme adopts
            // it, so a code that only styles dropped text never reaches the
            // result (`truncateToWidth("\x1b[4ma\x1b[31mbcdef", 4, "...")` is
            // "\x1b[4ma\x1b[0m...\x1b[0m").
            pending_ansi += token.text;
            continue;
        }
        if (collected_width + token.width > target_width) break;
        result += pending_ansi;
        pending_ansi.clear();
        result += token.text;
        collected_width += token.width;
    }
    return finalize_truncation(std::move(result), collected_width, ellipsis, ellipsis_width, max_width, pad);
}

support::Expected<std::string> slice_by_column(
        std::string_view line, std::size_t start_col, std::size_t length, bool strict) {
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
                if (current_col == start_col && !pending_ansi.empty()) {
                    result += pending_ansi;
                    pending_ansi.clear();
                }
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

std::optional<std::string> osc8_link_at_column(std::string_view line, std::size_t column) {
    auto tokens = detail::tokenize_terminal_output(line);
    if (!tokens) return std::nullopt;

    std::optional<std::string> active_link;
    std::size_t current_col = 0;
    for (const auto& token : *tokens) {
        if (token.kind == detail::TerminalTokenKind::Newline) continue;
        if (token.kind != detail::TerminalTokenKind::Grapheme) {
            // pi keeps only the URL of an OSC 8 open/close; an empty URL closes
            // the active link (`\x1b]8;;\x07`).
            std::string_view body(token.text);
            const auto terminator_size = body.ends_with('\x07') ? 1U : 2U;
            if (body.size() > 4 + terminator_size && body.starts_with("\x1b]8;")) {
                body = body.substr(4, body.size() - 4 - terminator_size);
                const auto separator = body.find(';');
                if (separator != std::string_view::npos) {
                    const auto url = body.substr(separator + 1);
                    active_link = url.empty() ? std::nullopt : std::optional<std::string>{std::string(url)};
                }
            }
            continue;
        }
        if (column >= current_col && column < current_col + token.width) return active_link;
        current_col += token.width;
    }
    return std::nullopt;
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
                while (cursor < text.size() && text[cursor] != 'm' && text[cursor] != 'G' && text[cursor] != 'K' &&
                        text[cursor] != 'H' && text[cursor] != 'J') {
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

namespace {

/// pi expands a pasted TAB to this many columns in both the multiline editor
/// and the single-line input.
constexpr std::size_t kPasteTabColumns = 4;

/// Modifier bits of a `CSI u` reply, matching the decoder in `tui/InputDecoder.cpp`.
constexpr unsigned int kPasteShiftModifier = 1;
constexpr unsigned int kPasteAltModifier = 2;
constexpr unsigned int kPasteCtrlModifier = 4;
constexpr unsigned int kPasteSuperModifier = 8;
constexpr unsigned int kPasteLockModifiers = 64 + 128;
constexpr unsigned int kPasteKnownModifiers =
        kPasteShiftModifier | kPasteAltModifier | kPasteCtrlModifier | kPasteSuperModifier;

struct ParsedDecimal {
    unsigned int value{0};
    bool valid{false};
};

[[nodiscard]] ParsedDecimal parse_decimal(std::string_view text) {
    if (text.empty()) return {};
    unsigned int value = 0;
    for (const char character : text) {
        if (character < '0' || character > '9') return {};
        value = value * 10 + static_cast<unsigned int>(character - '0');
        if (value > 0x10ffff) return {};
    }
    return {.value = value, .valid = true};
}

[[nodiscard]] bool is_decimal_digit(char character) { return character >= '0' && character <= '9'; }

[[nodiscard]] std::vector<std::string_view> split_on(std::string_view text, char separator) {
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    while (true) {
        const auto position = text.find(separator, start);
        parts.push_back(
                text.substr(start, position == std::string_view::npos ? text.size() - start : position - start));
        if (position == std::string_view::npos) return parts;
        start = position + 1;
    }
}

[[nodiscard]] std::string encode_utf8(char32_t codepoint) {
    std::string encoded;
    if (codepoint <= 0x7f) {
        encoded.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7ff) {
        encoded.push_back(static_cast<char>(0xc0 | (codepoint >> 6)));
        encoded.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    } else if (codepoint <= 0xffff) {
        encoded.push_back(static_cast<char>(0xe0 | (codepoint >> 12)));
        encoded.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
        encoded.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    } else if (codepoint <= 0x10ffff) {
        encoded.push_back(static_cast<char>(0xf0 | (codepoint >> 18)));
        encoded.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f)));
        encoded.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
        encoded.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    }
    return encoded;
}

/// The character a control reply encodes, with the kitty modifier mapping
/// applied. Ctrl+J (codepoint 106) therefore resolves to LF, Ctrl+I to TAB and
/// Ctrl+H to a backspace byte that the control filter removes.
[[nodiscard]] char32_t resolve_reply_character(unsigned int codepoint, bool shift, bool ctrl) {
    auto resolved = static_cast<char32_t>(codepoint);
    if (shift && resolved >= 'a' && resolved <= 'z') resolved -= 'a' - 'A';
    if (!ctrl) return resolved;
    if (resolved >= 'a' && resolved <= 'z') return resolved - 96;
    if (resolved >= 'A' && resolved <= 'Z') return resolved - 64;
    if (resolved == ' ') return 0x00;
    if (resolved >= '[' && resolved <= '_') return resolved - '@';
    if (resolved == '?') return 0x7f;
    return resolved;
}

struct PasteControlReply {
    std::size_t consumed{0};
    /// True when the bytes are a complete `CSI u` reply, including the repeat
    /// and release forms that carry no insertable character.
    bool recognized{false};
    bool press{true};
    char32_t character{0};
};

/// Parse a complete `ESC [ <parameters> u` reply at `position`. Anything else
/// (other CSI forms, truncated bytes) stays unconsumed and is filtered as
/// ordinary control bytes.
[[nodiscard]] PasteControlReply parse_paste_control_reply(std::string_view text, std::size_t position) {
    if (position + 1 >= text.size() || text[position] != '\x1b' || text[position + 1] != '[') return {};
    const std::size_t body_start = position + 2;
    std::size_t cursor = body_start;
    while (cursor < text.size() && (is_decimal_digit(text[cursor]) || text[cursor] == ';' || text[cursor] == ':')) {
        ++cursor;
    }
    if (cursor >= text.size() || text[cursor] != 'u') return {};

    const auto body = text.substr(body_start, cursor - body_start);
    const auto sections = split_on(body, ';');
    if (sections.empty() || sections.size() > 2) return {};
    const auto key_parts = split_on(sections[0], ':');
    if (key_parts.empty() || key_parts.size() > 3) return {};
    const auto codepoint = parse_decimal(key_parts[0]);
    if (!codepoint.valid) return {};

    unsigned int modifier_value = 1;
    unsigned int event_type = 1;
    if (sections.size() == 2) {
        const auto modifier_parts = split_on(sections[1], ':');
        if (modifier_parts.empty() || modifier_parts.size() > 2) return {};
        const auto parsed_modifier = parse_decimal(modifier_parts[0]);
        if (!parsed_modifier.valid || parsed_modifier.value == 0) return {};
        modifier_value = parsed_modifier.value;
        if (modifier_parts.size() == 2) {
            const auto parsed_event = parse_decimal(modifier_parts[1]);
            if (!parsed_event.valid) return {};
            event_type = parsed_event.value;
        }
    }
    const auto modifier = (modifier_value - 1) & ~kPasteLockModifiers;
    if ((modifier & ~kPasteKnownModifiers) != 0) return {};

    return PasteControlReply{
            .consumed = cursor + 1 - position,
            .recognized = true,
            .press = event_type != 2 && event_type != 3,
            .character = resolve_reply_character(
                    codepoint.value, (modifier & kPasteShiftModifier) != 0, (modifier & kPasteCtrlModifier) != 0),
    };
}

[[nodiscard]] bool is_filtered_control(char32_t codepoint) {
    return codepoint < 0x20 || codepoint == 0x7f || (codepoint >= 0x80 && codepoint <= 0x9f);
}

/// Whitespace and filtered controls both end the current token, which is what
/// lets a following path start a new token.
[[nodiscard]] bool ends_token(char32_t codepoint) {
    return codepoint == ' ' || codepoint == '\t' || codepoint == '\n' || codepoint == '\r' ||
           is_filtered_control(codepoint);
}

/// ASCII word characters only: pasted spacing must never be inserted inside
/// CJK or other scriptio continua, which carry no inter-word spaces.
[[nodiscard]] bool is_word_character(char character) {
    const auto byte = static_cast<unsigned char>(character);
    return (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
           byte == '_' || byte == '-';
}

[[nodiscard]] bool is_path_trailing_character(char character) {
    const auto byte = static_cast<unsigned char>(character);
    return is_word_character(character) || byte == '.' || byte == '/' || byte == '~';
}

[[nodiscard]] bool starts_path_token(std::string_view text, std::size_t position) {
    return text[position] == '/' || text.substr(position, 2) == "./" || text.substr(position, 2) == "~/" ||
           text.substr(position, 3) == "../";
}

} // namespace

std::string normalize_pasted_text(std::string_view text, PasteNormalization normalization) {
    const bool multiline = normalization.multiline;
    const std::string_view before = normalization.boundaries.text_before;
    const std::string_view after = normalization.boundaries.text_after;

    std::string result;
    result.reserve(text.size());

    // The nearest existing character participates in the path-spacing rule, so
    // the pasted text and the surrounding text are spaced identically.
    char previous_character = before.empty() ? '\0' : before.back();
    bool at_token_start = before.empty() || ends_token(static_cast<unsigned char>(before.back()));
    // A path token stays one token until whitespace ends it, so the separators
    // inside "/tmp/x" are never mistaken for a path glued to a word.
    bool in_path_token = false;
    // CRLF must collapse to one LF, so the LF of a CR LF pair is consumed.
    bool after_carriage_return = false;

    const auto emit = [&](char32_t codepoint) {
        if (codepoint == '\r' || codepoint == '\n') {
            if (multiline) {
                result.push_back('\n');
                previous_character = '\n';
            }
            return;
        }
        if (codepoint == '\t') {
            result.append(kPasteTabColumns, ' ');
            previous_character = ' ';
            return;
        }
        if (is_filtered_control(codepoint)) return;
        result += encode_utf8(codepoint);
        previous_character = result.back();
    };

    for (std::size_t index = 0; index < text.size();) {
        if (const auto reply = parse_paste_control_reply(text, index); reply.recognized) {
            if (reply.press) {
                emit(reply.character);
                after_carriage_return = reply.character == '\r';
            }
            at_token_start = ends_token(reply.character);
            in_path_token = false;
            index += reply.consumed;
            continue;
        }
        const auto [codepoint, bytes] = detail::decode_utf8(text, index);
        if (after_carriage_return && codepoint == '\n') {
            after_carriage_return = false;
            at_token_start = true;
            index += bytes;
            continue;
        }
        const bool starts_path = !in_path_token && starts_path_token(text, index);
        if (starts_path && is_word_character(previous_character)) {
            result.push_back(' ');
            previous_character = ' ';
        }
        emit(codepoint);
        after_carriage_return = codepoint == '\r';
        at_token_start = ends_token(codepoint);
        in_path_token = !at_token_start && (in_path_token || starts_path);
        index += bytes;
    }

    // The pasted path's own end is spaced against text that follows it.
    if (in_path_token && !result.empty() && is_path_trailing_character(result.back()) && !after.empty() &&
            is_word_character(after.front())) {
        result.push_back(' ');
    }
    return result;
}

} // namespace cch::tui

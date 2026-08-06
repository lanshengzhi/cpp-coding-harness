#include <cch/tui/Utils.hpp>

#include "tui/UnicodeWidth.hpp"

#include <utf8proc.h>

#include <algorithm>
#include <cstddef>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::tui {
namespace {

/// The ANSI SGR reset pi's truncateToWidth always emits around the ellipsis.
constexpr std::string_view kSgrReset{"\x1b[0m"};

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

util::Expected<std::vector<std::string>> wrap_text(std::string_view text, std::size_t width) {
    if (width == 0) {
        return std::unexpected(detail::invalid_terminal_text("Wrap width must be positive"));
    }
    auto tokens = detail::tokenize_terminal_output(text);
    if (!tokens) return std::unexpected(tokens.error());

    const auto is_whitespace = [](const detail::TerminalToken& token) {
        if (token.kind != detail::TerminalTokenKind::Grapheme) return false;
        const auto [codepoint, bytes] = detail::decode_utf8(token.text, 0);
        if (bytes == 0) return false;
        const auto category = utf8proc_category(static_cast<utf8proc_int32_t>(codepoint));
        return codepoint == ' ' || category == UTF8PROC_CATEGORY_ZS ||
               category == UTF8PROC_CATEGORY_ZL || category == UTF8PROC_CATEGORY_ZP;
    };

    std::vector<std::string> lines;
    std::vector<detail::TerminalToken> pending_separator;
    detail::AnsiStyleState style;
    std::string line;
    std::size_t line_width = 0;
    std::size_t pending_width = 0;

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
    const auto finish_line = [&]() {
        line += style.get_line_end_reset();
        lines.push_back(std::move(line));
        line = style.get_active_codes();
        line_width = 0;
    };
    const auto replay_pending = [&](bool keep_whitespace) {
        for (const auto& pending : pending_separator) {
            if (keep_whitespace || pending.kind != detail::TerminalTokenKind::Grapheme) {
                append_token(pending);
            }
        }
        clear_pending();
    };
    const auto finish_before_pending = [&]() {
        finish_line();
        replay_pending(false);
    };

    std::size_t index = 0;
    while (index < tokens->size()) {
        const auto& token = (*tokens)[index];
        if (token.kind == detail::TerminalTokenKind::Newline) {
            if (line_width + pending_width <= width) replay_pending(true);
            else replay_pending(false);
            finish_line();
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

        auto word_end = index;
        std::size_t word_width = 0;
        std::size_t first_grapheme_width = 0;
        while (word_end < tokens->size()) {
            const auto& word_token = (*tokens)[word_end];
            if (word_token.kind == detail::TerminalTokenKind::Newline || is_whitespace(word_token)) break;
            if (first_grapheme_width == 0 && word_token.kind == detail::TerminalTokenKind::Grapheme) {
                first_grapheme_width = word_token.width;
            }
            word_width += word_token.width;
            ++word_end;
        }

        if (word_width <= width) {
            if (line_width + pending_width + word_width > width) {
                if (line_width != 0) finish_before_pending();
                else replay_pending(false);
            } else {
                replay_pending(true);
            }
            while (index < word_end) append_token((*tokens)[index++]);
            continue;
        }

        if (line_width + pending_width + first_grapheme_width <= width) {
            replay_pending(true);
        } else if (line_width != 0) {
            finish_before_pending();
        } else {
            replay_pending(false);
        }

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
            if (line_width != 0 && line_width + word_token.width > width) finish_line();
            append_token(word_token);
        }
    }

    if (line_width + pending_width <= width) replay_pending(true);
    else replay_pending(false);
    line += style.get_line_end_reset();
    lines.push_back(std::move(line));
    return lines;
}

util::Expected<std::string> truncate_text(
    std::string_view text,
    std::size_t max_width,
    std::string_view ellipsis,
    bool pad) {
    if (max_width == 0) return std::string{};

    auto tokens = detail::tokenize_terminal_output(text);
    if (!tokens) return std::unexpected(tokens.error());
    auto width_result = detail::token_width(*tokens);
    if (!width_result) return std::unexpected(width_result.error());

    detail::AnsiStyleState style;
    if (*width_result <= max_width) {
        auto result = detail::normalized_text(*tokens);
        for (const auto& token : *tokens) {
            if (token.kind != detail::TerminalTokenKind::Grapheme) style.process_ansi(token.text);
        }
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
            style.process_ansi(token.text);
            continue;
        }
        if (collected_width + token.width > target_width) break;
        result += token.text;
        collected_width += token.width;
    }
    // pi's finalizeTruncatedResult always wraps the ellipsis in SGR resets,
    // styled or not (utils.ts at the frozen baseline); the line-end reset
    // already closes active styles, so only add the pre reset when nothing
    // is active.
    result += style.get_line_end_reset();
    if (!result.ends_with(kSgrReset)) result += kSgrReset;
    result += ellipsis;
    if (!ellipsis.empty()) result += kSgrReset;
    if (pad) result.append(max_width - collected_width - ellipsis_width, ' ');
    return result;
}

util::Expected<std::string> slice_by_column(
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

#include "DiffRenderer.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::coding_agent::tui {
namespace {

struct ParsedDiffLine {
    char prefix{};
    std::string line_number;
    std::string content;
};

/// pi `diff.ts` `parseDiffLine`: `^([+-\s])(\s*\d*)\s(.*)$`. The greedy
/// `\s*` backtracks, so the required `\s` may sit anywhere after the prefix
/// as long as the region between them is whitespace-then-digits.
[[nodiscard]] std::optional<ParsedDiffLine> parse_diff_line(std::string_view line) {
    if (line.empty()) return std::nullopt;
    const char prefix = line[0];
    if (prefix != '+' && prefix != '-' && prefix != ' ' && prefix != '\t') {
        return std::nullopt;
    }
    const auto is_whitespace = [](char value) {
        return value == ' ' || value == '\t';
    };
    std::size_t run_end = 1;
    while (run_end < line.size() && is_whitespace(line[run_end])) ++run_end;
    // Try `\s*` lengths from maximal down to zero; the first success is the
    // regex's greedy choice.
    for (std::size_t w1_len = run_end - 1;; --w1_len) {
        std::size_t cursor = 1 + w1_len;
        const auto digits_begin = cursor;
        while (cursor < line.size() && line[cursor] >= '0' && line[cursor] <= '9') ++cursor;
        if (cursor < line.size() && is_whitespace(line[cursor])) {
            return ParsedDiffLine{
                .prefix = prefix,
                .line_number = std::string{line.substr(digits_begin, cursor - digits_begin)},
                .content = std::string{line.substr(cursor + 1)},
            };
        }
        if (w1_len == 0) break;
    }
    return std::nullopt;
}

/// pi `diff.ts` `replaceTabs`: tabs expand to three spaces.
[[nodiscard]] std::string replace_tabs(std::string_view text) {
    std::string result;
    result.reserve(text.size());
    for (const char value : text) {
        if (value == '\t') {
            result += "   ";
        } else {
            result.push_back(value);
        }
    }
    return result;
}

/// jsdiff `diffWords` tokenization: split on whitespace runs, then attach
/// each whitespace run to the following token so whitespace moves with its
/// word.
[[nodiscard]] std::vector<std::string> word_tokens(std::string_view text) {
    std::vector<std::string> tokens;
    std::size_t start = 0;
    const auto is_whitespace = [](char value) {
        return value == ' ' || value == '\t' || value == '\r' || value == '\n';
    };
    while (start < text.size()) {
        std::size_t end = start;
        while (end < text.size() && is_whitespace(text[end])) ++end;
        if (end > start) tokens.emplace_back(text.substr(start, end - start));
        if (end >= text.size()) break;
        start = end;
        while (end < text.size() && !is_whitespace(text[end])) ++end;
        tokens.emplace_back(text.substr(start, end - start));
        start = end;
    }
    std::vector<std::string> merged;
    merged.reserve(tokens.size());
    for (std::size_t index = 0; index < tokens.size(); ++index) {
        if (index + 1 < tokens.size() && is_whitespace(tokens[index][0]) &&
            !is_whitespace(tokens[index + 1][0])) {
            merged.push_back(tokens[index] + tokens[index + 1]);
            ++index;
        } else {
            merged.push_back(std::move(tokens[index]));
        }
    }
    return merged;
}

/// Longest-common-subsequence diff over word tokens; returns jsdiff-style
/// parts where `removed`/`added` flags mark changed token runs.
struct WordPart {
    std::string value;
    bool removed{false};
    bool added{false};
};

[[nodiscard]] std::vector<WordPart> word_diff(
    const std::vector<std::string>& old_tokens,
    const std::vector<std::string>& new_tokens) {
    const auto rows = old_tokens.size() + 1;
    const auto columns = new_tokens.size() + 1;
    std::vector<int> lengths(rows * columns, 0);
    for (std::size_t row = 1; row < rows; ++row) {
        for (std::size_t column = 1; column < columns; ++column) {
            lengths[row * columns + column] =
                old_tokens[row - 1] == new_tokens[column - 1]
                ? lengths[(row - 1) * columns + column - 1] + 1
                : std::max(lengths[(row - 1) * columns + column],
                           lengths[row * columns + column - 1]);
        }
    }

    std::vector<WordPart> parts;
    std::size_t row = old_tokens.size();
    std::size_t column = new_tokens.size();
    while (row > 0 && column > 0) {
        if (old_tokens[row - 1] == new_tokens[column - 1]) {
            parts.push_back({.value = old_tokens[row - 1]});
            --row;
            --column;
        } else if (lengths[(row - 1) * columns + column] >= lengths[row * columns + column - 1]) {
            parts.push_back({.value = old_tokens[row - 1], .removed = true});
            --row;
        } else {
            parts.push_back({.value = new_tokens[column - 1], .added = true});
            --column;
        }
    }
    while (row > 0) {
        parts.push_back({.value = old_tokens[row - 1], .removed = true});
        --row;
    }
    while (column > 0) {
        parts.push_back({.value = new_tokens[column - 1], .added = true});
        --column;
    }
    std::reverse(parts.begin(), parts.end());
    return parts;
}

/// pi `diff.ts` `renderIntraLineDiff`: inverse on changed parts, with the
/// leading whitespace of the first changed part kept plain.
struct IntraLineDiff {
    std::string removed_line;
    std::string added_line;
};

[[nodiscard]] std::string strip_leading_whitespace(std::string& value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    std::string whitespace = value.substr(0, first);
    value.erase(0, first);
    return whitespace;
}

[[nodiscard]] IntraLineDiff render_intra_line_diff(
    const LiveTheme& theme,
    std::string_view old_content,
    std::string_view new_content) {
    const auto parts = word_diff(word_tokens(old_content), word_tokens(new_content));
    IntraLineDiff result;
    bool first_removed = true;
    bool first_added = true;
    for (const auto& part : parts) {
        if (part.removed) {
            auto value = part.value;
            if (first_removed) {
                result.removed_line += strip_leading_whitespace(value);
                first_removed = false;
            }
            if (!value.empty()) {
                result.removed_line += std::format("\x1b[7m{}\x1b[27m", value);
            }
        } else if (part.added) {
            auto value = part.value;
            if (first_added) {
                result.added_line += strip_leading_whitespace(value);
                first_added = false;
            }
            if (!value.empty()) {
                result.added_line += std::format("\x1b[7m{}\x1b[27m", value);
            }
        } else {
            result.removed_line += part.value;
            result.added_line += part.value;
        }
    }
    return result;
}

[[nodiscard]] std::string styled_line(
    const LiveTheme& theme,
    ThemeToken token,
    std::string line) {
    return theme.foreground(token, std::move(line));
}

} // namespace

std::string render_diff(const LiveTheme& theme, std::string_view diff_text) {
    std::vector<std::string_view> lines;
    std::size_t start = 0;
    while (start < diff_text.size()) {
        const auto newline = diff_text.find('\n', start);
        const auto end = newline == std::string_view::npos ? diff_text.size() : newline;
        lines.emplace_back(diff_text.substr(start, end - start));
        if (newline == std::string_view::npos) break;
        start = newline + 1;
    }

    std::string result;
    const auto append = [&result](std::string line) {
        if (!result.empty()) result.push_back('\n');
        result += std::move(line);
    };

    std::size_t index = 0;
    while (index < lines.size()) {
        const auto parsed = parse_diff_line(lines[index]);
        if (!parsed || parsed->prefix != '-') {
            // Context line, or a standalone added line.
            if (parsed && parsed->prefix == '+') {
                append(styled_line(
                    theme,
                    ThemeToken::ToolDiffAdded,
                    "+" + parsed->line_number + " " + replace_tabs(parsed->content)));
            } else if (parsed) {
                append(styled_line(
                    theme,
                    ThemeToken::ToolDiffContext,
                    " " + parsed->line_number + " " + replace_tabs(parsed->content)));
            } else {
                append(styled_line(theme, ThemeToken::ToolDiffContext, std::string{lines[index]}));
            }
            ++index;
            continue;
        }

        // Collect consecutive removed lines, then consecutive added lines.
        struct DiffLine {
            std::string line_number;
            std::string content;
        };
        std::vector<DiffLine> removed_lines;
        while (index < lines.size()) {
            const auto candidate = parse_diff_line(lines[index]);
            if (!candidate || candidate->prefix != '-') break;
            removed_lines.push_back(DiffLine{
                .line_number = candidate->line_number,
                .content = candidate->content,
            });
            ++index;
        }
        std::vector<DiffLine> added_lines;
        while (index < lines.size()) {
            const auto candidate = parse_diff_line(lines[index]);
            if (!candidate || candidate->prefix != '+') break;
            added_lines.push_back(DiffLine{
                .line_number = candidate->line_number,
                .content = candidate->content,
            });
            ++index;
        }

        if (removed_lines.size() == 1 && added_lines.size() == 1) {
            // Single-line modification: word-level highlight.
            const auto intra = render_intra_line_diff(
                theme,
                replace_tabs(removed_lines[0].content),
                replace_tabs(added_lines[0].content));
            append(styled_line(
                theme,
                ThemeToken::ToolDiffRemoved,
                "-" + removed_lines[0].line_number + " " + intra.removed_line));
            append(styled_line(
                theme,
                ThemeToken::ToolDiffAdded,
                "+" + added_lines[0].line_number + " " + intra.added_line));
        } else {
            for (const auto& removed : removed_lines) {
                append(styled_line(
                    theme,
                    ThemeToken::ToolDiffRemoved,
                    "-" + removed.line_number + " " + replace_tabs(removed.content)));
            }
            for (const auto& added : added_lines) {
                append(styled_line(
                    theme,
                    ThemeToken::ToolDiffAdded,
                    "+" + added.line_number + " " + replace_tabs(added.content)));
            }
        }
    }
    return result;
}

} // namespace cch::coding_agent::tui

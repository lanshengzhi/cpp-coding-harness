#include "agent/tools/EditDiff.hpp"

#include <utf8proc.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::tools {
namespace {

[[nodiscard]] support::Error edit_error(std::string message) {
    return support::make_error(
        support::ErrorCode::Validation,
        std::move(message));
}

// ---------------------------------------------------------------------------
// Fuzzy-match normalization (pi edit-diff.ts normalizeForFuzzyMatch)
// ---------------------------------------------------------------------------

/// ECMAScript `String.prototype.normalize("NFKC")` via utf8proc. Falls back
/// to the input on invalid UTF-8 (JS normalizing also fails on such input).
[[nodiscard]] std::string nfkc_normalize(std::string_view text) {
    utf8proc_uint8_t* result = nullptr;
    const auto length = utf8proc_map(
        reinterpret_cast<const utf8proc_uint8_t*>(text.data()),
        static_cast<utf8proc_ssize_t>(text.size()),
        &result,
        static_cast<utf8proc_option_t>(
            UTF8PROC_COMPAT | UTF8PROC_COMPOSE));
    if (length < 0 || result == nullptr) {
        return std::string(text);
    }
    std::string normalized(
        reinterpret_cast<const char*>(result),
        static_cast<std::size_t>(length));
    std::free(result);
    return normalized;
}

/// One trailing UTF-8 whitespace unit at the end of `text`, or zero.
[[nodiscard]] std::size_t trailing_whitespace_unit(std::string_view text) {
    if (text.empty()) {
        return 0;
    }
    const auto last = static_cast<unsigned char>(text.back());
    if (last == ' ' || last == '\t' || last == '\v' || last == '\f' ||
        last == '\r') {
        return 1;
    }
    // Non-ASCII whitespace that NFKC leaves in place (ECMAScript WhiteSpace
    // minus the codepoints NFKC already decomposed to U+0020).
    constexpr std::array<std::string_view, 7> units{
        "\xc2\xa0",     // U+00A0 NBSP
        "\xe1\x9a\x80", // U+1680 OGHAM SPACE MARK
        "\xe2\x80\xa8", // U+2028 LINE SEPARATOR
        "\xe2\x80\xa9", // U+2029 PARAGRAPH SEPARATOR
        "\xe2\x80\xaf", // U+202F NARROW NBSP
        "\xe2\x81\x9f", // U+205F MEDIUM MATHEMATICAL SPACE
        "\xef\xbb\xbf", // U+FEFF ZERO WIDTH NO-BREAK SPACE
    };
    for (const auto unit : units) {
        if (text.ends_with(unit)) {
            return unit.size();
        }
    }
    // U+2000..U+200A EN/EM/.. spaces: NFKC decomposes most, but keep the
    // explicit range for hosts whose utf8proc version differs.
    if (text.size() >= 3 &&
        static_cast<unsigned char>(text[text.size() - 3]) == 0xe2 &&
        static_cast<unsigned char>(text[text.size() - 2]) == 0x80 &&
        static_cast<unsigned char>(text[text.size() - 1]) >= 0x80 &&
        static_cast<unsigned char>(text[text.size() - 1]) <= 0x8a) {
        return 3;
    }
    return 0;
}

void replace_all(std::string& text, std::string_view needle, std::string_view replacement) {
    std::size_t position = 0;
    while ((position = text.find(needle, position)) != std::string::npos) {
        text.replace(position, needle.size(), replacement);
        position += replacement.size();
    }
}

/// pi `normalizeForFuzzyMatch`: NFKC, per-line trailing whitespace stripped,
/// smart quotes/dashes/special spaces normalized to ASCII equivalents.
[[nodiscard]] std::string normalize_for_fuzzy_match(std::string_view text) {
    std::string normalized = nfkc_normalize(text);

    std::string trimmed;
    trimmed.reserve(normalized.size());
    std::size_t line_start = 0;
    while (line_start <= normalized.size()) {
        const auto line_end = normalized.find('\n', line_start);
        const auto end = line_end == std::string::npos
                             ? normalized.size()
                             : line_end;
        auto line = std::string_view(normalized).substr(line_start, end - line_start);
        while (const auto unit = trailing_whitespace_unit(line)) {
            line.remove_suffix(unit);
        }
        trimmed.append(line);
        if (line_end != std::string::npos) {
            trimmed.push_back('\n');
            line_start = line_end + 1;
        } else {
            break;
        }
    }

    replace_all(trimmed, "\xe2\x80\x98", "'"); // U+2018
    replace_all(trimmed, "\xe2\x80\x99", "'"); // U+2019
    replace_all(trimmed, "\xe2\x80\x9a", "'"); // U+201A
    replace_all(trimmed, "\xe2\x80\x9b", "'"); // U+201B
    replace_all(trimmed, "\xe2\x80\x9c", "\""); // U+201C
    replace_all(trimmed, "\xe2\x80\x9d", "\""); // U+201D
    replace_all(trimmed, "\xe2\x80\x9e", "\""); // U+201E
    replace_all(trimmed, "\xe2\x80\x9f", "\""); // U+201F
    replace_all(trimmed, "\xe2\x80\x90", "-");  // U+2010
    replace_all(trimmed, "\xe2\x80\x91", "-");  // U+2011
    replace_all(trimmed, "\xe2\x80\x92", "-");  // U+2012
    replace_all(trimmed, "\xe2\x80\x93", "-");  // U+2013
    replace_all(trimmed, "\xe2\x80\x94", "-");  // U+2014
    replace_all(trimmed, "\xe2\x80\x95", "-");  // U+2015
    replace_all(trimmed, "\xe2\x88\x92", "-");  // U+2212
    replace_all(trimmed, "\xc2\xa0", " ");      // U+00A0
    replace_all(trimmed, "\xe2\x80\x82", " ");  // U+2002
    replace_all(trimmed, "\xe2\x80\x83", " ");  // U+2003
    replace_all(trimmed, "\xe2\x80\x84", " ");  // U+2004
    replace_all(trimmed, "\xe2\x80\x85", " ");  // U+2005
    replace_all(trimmed, "\xe2\x80\x86", " ");  // U+2006
    replace_all(trimmed, "\xe2\x80\x87", " ");  // U+2007
    replace_all(trimmed, "\xe2\x80\x88", " ");  // U+2008
    replace_all(trimmed, "\xe2\x80\x89", " ");  // U+2009
    replace_all(trimmed, "\xe2\x80\x8a", " ");  // U+200A
    replace_all(trimmed, "\xe2\x80\xaf", " ");  // U+202F
    replace_all(trimmed, "\xe2\x81\x9f", " ");  // U+205F
    replace_all(trimmed, "\xe3\x80\x80", " ");  // U+3000
    return trimmed;
}

struct FuzzyMatchResult {
    bool found{false};
    std::size_t index{0};
    std::size_t match_length{0};
    bool used_fuzzy_match{false};
};

/// pi `fuzzyFindText`: exact match first, then fuzzy-normalized matching.
[[nodiscard]] FuzzyMatchResult fuzzy_find_text(
    std::string_view content,
    std::string_view old_text) {
    const auto exact_index = content.find(old_text);
    if (exact_index != std::string_view::npos) {
        return FuzzyMatchResult{
            .found = true,
            .index = exact_index,
            .match_length = old_text.size(),
            .used_fuzzy_match = false,
        };
    }
    const auto fuzzy_content = normalize_for_fuzzy_match(content);
    const auto fuzzy_old_text = normalize_for_fuzzy_match(old_text);
    const auto fuzzy_index = fuzzy_content.find(fuzzy_old_text);
    if (fuzzy_index == std::string::npos) {
        return FuzzyMatchResult{};
    }
    return FuzzyMatchResult{
        .found = true,
        .index = fuzzy_index,
        .match_length = fuzzy_old_text.size(),
        .used_fuzzy_match = true,
    };
}

/// pi `countOccurrences`: `fuzzyContent.split(fuzzyOldText).length - 1`.
[[nodiscard]] std::size_t count_occurrences(
    std::string_view content,
    std::string_view old_text) {
    const auto fuzzy_content = normalize_for_fuzzy_match(content);
    const auto fuzzy_old_text = normalize_for_fuzzy_match(old_text);
    std::size_t count = 0;
    std::size_t position = 0;
    while ((position = fuzzy_content.find(fuzzy_old_text, position)) !=
           std::string_view::npos) {
        ++count;
        position += fuzzy_old_text.size();
    }
    return count;
}

// ---------------------------------------------------------------------------
// Line spans and replacement application (pi edit-diff.ts)
// ---------------------------------------------------------------------------

/// pi `splitLinesWithEndings`: every line keeps its trailing newline.
[[nodiscard]] std::vector<std::string> split_lines_with_endings(std::string_view content) {
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start < content.size()) {
        const auto newline = content.find('\n', start);
        if (newline == std::string_view::npos) {
            lines.emplace_back(content.substr(start));
            break;
        }
        lines.emplace_back(content.substr(start, newline - start + 1));
        start = newline + 1;
    }
    return lines;
}

struct LineSpan {
    std::size_t start{0};
    std::size_t end{0};
};

[[nodiscard]] std::vector<LineSpan> get_line_spans(std::string_view content) {
    std::vector<LineSpan> spans;
    std::size_t offset = 0;
    for (const auto& line : split_lines_with_endings(content)) {
        const auto end = offset + line.size();
        spans.push_back(LineSpan{offset, end});
        offset = end;
    }
    return spans;
}

struct TextReplacement {
    std::size_t match_index{0};
    std::size_t match_length{0};
    std::string new_text;
};

struct MatchedEdit {
    std::size_t edit_index{0};
    std::size_t match_index{0};
    std::size_t match_length{0};
    std::string new_text;
};

struct LineRange {
    std::size_t start_line{0};
    std::size_t end_line{0};
};

/// pi `getReplacementLineRange`. Callers guarantee the replacement lies inside
/// the base content, so the "outside" errors are unreachable for our inputs.
[[nodiscard]] LineRange replacement_line_range(
    const std::vector<LineSpan>& lines,
    const TextReplacement& replacement) {
    const auto replacement_start = replacement.match_index;
    const auto replacement_end =
        replacement.match_index + replacement.match_length;

    std::size_t start_line = 0;
    for (; start_line < lines.size(); ++start_line) {
        const auto& line = lines[start_line];
        if (replacement_start >= line.start && replacement_start < line.end) {
            break;
        }
    }
    std::size_t end_line = start_line;
    while (end_line < lines.size() && lines[end_line].end < replacement_end) {
        ++end_line;
    }
    return LineRange{start_line, end_line + 1};
}

/// pi `applyReplacements`: replacements matched against `content`, applied in
/// reverse order so offsets stay stable.
[[nodiscard]] std::string apply_replacements(
    std::string_view content,
    const std::vector<TextReplacement>& replacements,
    std::size_t offset = 0) {
    std::string result(content);
    for (auto it = replacements.rbegin(); it != replacements.rend(); ++it) {
        const auto match_index = it->match_index - offset;
        result.replace(
            match_index,
            it->match_length,
            it->new_text);
    }
    return result;
}

/// pi `applyReplacementsPreservingUnchangedLines`. Returns nullopt when the
/// base content's line count differs from the original (pi throws).
[[nodiscard]] std::optional<std::string> apply_replacements_preserving_unchanged_lines(
    std::string_view original_content,
    std::string_view base_content,
    const std::vector<TextReplacement>& replacements) {
    const auto original_lines = split_lines_with_endings(original_content);
    const auto base_lines = get_line_spans(base_content);
    if (original_lines.size() != base_lines.size()) {
        return std::nullopt;
    }

    struct ReplacementGroup {
        std::size_t start_line{0};
        std::size_t end_line{0};
        std::vector<TextReplacement> replacements;
    };

    std::vector<ReplacementGroup> groups;
    auto sorted = replacements;
    std::stable_sort(sorted.begin(), sorted.end(), [](const TextReplacement& left, const TextReplacement& right) {
        return left.match_index < right.match_index;
    });
    for (const auto& replacement : sorted) {
        const auto range = replacement_line_range(base_lines, replacement);
        if (!groups.empty()) {
            auto& current = groups.back();
            if (range.start_line < current.end_line) {
                current.end_line = std::max(current.end_line, range.end_line);
                current.replacements.push_back(replacement);
                continue;
            }
        }
        groups.push_back(ReplacementGroup{
            .start_line = range.start_line,
            .end_line = range.end_line,
            .replacements = {replacement},
        });
    }

    std::size_t original_line_index = 0;
    std::string result;
    for (const auto& group : groups) {
        for (auto index = original_line_index; index < group.start_line; ++index) {
            result += original_lines[index];
        }
        const auto group_start_offset = base_lines[group.start_line].start;
        const auto group_end_offset = base_lines[group.end_line - 1].end;
        result += apply_replacements(
            base_content.substr(group_start_offset, group_end_offset - group_start_offset),
            group.replacements,
            group_start_offset);
        original_line_index = group.end_line;
    }
    for (auto index = original_line_index; index < original_lines.size(); ++index) {
        result += original_lines[index];
    }
    return result;
}

// ---------------------------------------------------------------------------
// Line-based diff (jsdiff diffLines semantics for the display and patch)
// ---------------------------------------------------------------------------

enum class DiffPartKind {
    Common,
    Added,
    Removed,
};

struct DiffPart {
    DiffPartKind kind{DiffPartKind::Common};
    std::vector<std::string> lines;
};

/// LCS length row over b[blo..bhi) after processing a[alo..ahi).
[[nodiscard]] std::vector<int> lcs_row(
    const std::vector<std::string>& a,
    std::size_t alo,
    std::size_t ahi,
    const std::vector<std::string>& b,
    std::size_t blo,
    std::size_t bhi) {
    const std::size_t width = bhi - blo;
    std::vector<int> prev(width + 1, 0);
    std::vector<int> cur(width + 1, 0);
    for (std::size_t i = alo; i < ahi; ++i) {
        for (std::size_t j = 0; j < width; ++j) {
            if (a[i] == b[blo + j]) {
                cur[j + 1] = prev[j] + 1;
            } else {
                cur[j + 1] = std::max(prev[j + 1], cur[j]);
            }
        }
        prev.swap(cur);
        std::fill(cur.begin(), cur.end(), 0);
    }
    return prev;
}

/// LCS length row over b suffixes: result[k] is the LCS length of
/// a[amid..ahi) and b[blo+k..bhi).
[[nodiscard]] std::vector<int> lcs_row_reversed(
    const std::vector<std::string>& a,
    std::size_t alo,
    std::size_t ahi,
    const std::vector<std::string>& b,
    std::size_t blo,
    std::size_t bhi) {
    const std::size_t width = bhi - blo;
    std::vector<int> prev(width + 1, 0);
    std::vector<int> cur(width + 1, 0);
    for (std::size_t i = ahi; i-- > alo;) {
        for (std::size_t j = width; j-- > 0;) {
            if (a[i] == b[blo + j]) {
                cur[j] = prev[j + 1] + 1;
            } else {
                cur[j] = std::max(prev[j], cur[j + 1]);
            }
        }
        prev.swap(cur);
        std::fill(cur.begin(), cur.end(), 0);
    }
    return prev;
}

/// Hirschberg reconstruction of the LCS index pairs (a index, b index).
void lcs_reconstruct(
    const std::vector<std::string>& a,
    std::size_t alo,
    std::size_t ahi,
    const std::vector<std::string>& b,
    std::size_t blo,
    std::size_t bhi,
    std::vector<std::pair<std::size_t, std::size_t>>& out) {
    if (alo >= ahi || blo >= bhi) {
        return;
    }
    if (ahi - alo == 1) {
        for (std::size_t j = blo; j < bhi; ++j) {
            if (a[alo] == b[j]) {
                out.emplace_back(alo, j);
                break;
            }
        }
        return;
    }
    const auto amid = alo + (ahi - alo) / 2;
    const auto forward = lcs_row(a, alo, amid, b, blo, bhi);
    // backward[k] = LCS(a[amid..ahi), b[blo+k..bhi)): suffix-based, so the
    // Hirschberg split maximizes forward[k] + backward[k].
    const auto backward = lcs_row_reversed(a, amid, ahi, b, blo, bhi);
    const std::size_t width = bhi - blo;
    std::size_t best_k = 0;
    int best_score = -1;
    for (std::size_t k = 0; k <= width; ++k) {
        const int score = forward[k] + backward[k];
        if (score > best_score) {
            best_score = score;
            best_k = k;
        }
    }
    lcs_reconstruct(a, alo, amid, b, blo, blo + best_k, out);
    lcs_reconstruct(a, amid, ahi, b, blo + best_k, bhi, out);
}

/// The quadratic LCS window is bounded; beyond it a common-prefix/suffix
/// trim plus full-replacement middle keeps large-file diffs correct and cheap.
inline constexpr std::size_t kMaxLcsWindow = 2048;

/// jsdiff `diffLines` parts: each line keeps its trailing newline.
[[nodiscard]] std::vector<DiffPart> diff_lines(
    std::string_view old_content,
    std::string_view new_content) {
    const auto old_lines = split_lines_with_endings(old_content);
    const auto new_lines = split_lines_with_endings(new_content);

    std::size_t prefix = 0;
    while (prefix < old_lines.size() && prefix < new_lines.size() &&
           old_lines[prefix] == new_lines[prefix]) {
        ++prefix;
    }
    std::size_t suffix_old = old_lines.size();
    std::size_t suffix_new = new_lines.size();
    while (suffix_old > prefix && suffix_new > prefix &&
           old_lines[suffix_old - 1] == new_lines[suffix_new - 1]) {
        --suffix_old;
        --suffix_new;
    }

    std::vector<DiffPart> parts;
    auto push_lines = [&](DiffPartKind kind, const std::vector<std::string>& lines) {
        if (lines.empty()) {
            return;
        }
        if (!parts.empty() && parts.back().kind == kind) {
            parts.back().lines.insert(
                parts.back().lines.end(), lines.begin(), lines.end());
            return;
        }
        parts.push_back(DiffPart{kind, lines});
    };

    const std::size_t middle_old = suffix_old - prefix;
    const std::size_t middle_new = suffix_new - prefix;
    const bool bounded =
        middle_old <= kMaxLcsWindow && middle_new <= kMaxLcsWindow;

    std::vector<std::pair<std::size_t, std::size_t>> lcs;
    if (bounded) {
        lcs_reconstruct(
            old_lines, prefix, suffix_old, new_lines, prefix, suffix_new, lcs);
    }

    std::size_t old_index = prefix;
    std::size_t new_index = prefix;
    push_lines(
        DiffPartKind::Common,
        std::vector<std::string>(
            old_lines.begin(),
            old_lines.begin() + static_cast<std::ptrdiff_t>(prefix)));
    if (bounded) {
        for (const auto& [old_match, new_match] : lcs) {
            push_lines(
                DiffPartKind::Removed,
                std::vector<std::string>(
                    old_lines.begin() + static_cast<std::ptrdiff_t>(old_index),
                    old_lines.begin() + static_cast<std::ptrdiff_t>(old_match)));
            push_lines(
                DiffPartKind::Added,
                std::vector<std::string>(
                    new_lines.begin() + static_cast<std::ptrdiff_t>(new_index),
                    new_lines.begin() + static_cast<std::ptrdiff_t>(new_match)));
            push_lines(DiffPartKind::Common, {old_lines[old_match]});
            old_index = old_match + 1;
            new_index = new_match + 1;
        }
        push_lines(
            DiffPartKind::Removed,
            std::vector<std::string>(
                old_lines.begin() + static_cast<std::ptrdiff_t>(old_index),
                old_lines.begin() + static_cast<std::ptrdiff_t>(suffix_old)));
        push_lines(
            DiffPartKind::Added,
            std::vector<std::string>(
                new_lines.begin() + static_cast<std::ptrdiff_t>(new_index),
                new_lines.begin() + static_cast<std::ptrdiff_t>(suffix_new)));
    } else {
        push_lines(
            DiffPartKind::Removed,
            std::vector<std::string>(
                old_lines.begin() + static_cast<std::ptrdiff_t>(prefix),
                old_lines.begin() + static_cast<std::ptrdiff_t>(suffix_old)));
        push_lines(
            DiffPartKind::Added,
            std::vector<std::string>(
                new_lines.begin() + static_cast<std::ptrdiff_t>(prefix),
                new_lines.begin() + static_cast<std::ptrdiff_t>(suffix_new)));
    }
    push_lines(
        DiffPartKind::Common,
        std::vector<std::string>(
            old_lines.begin() + static_cast<std::ptrdiff_t>(suffix_old),
            old_lines.end()));
    return parts;
}

} // namespace

std::string_view detect_line_ending(std::string_view content) {
    const auto crlf_index = content.find("\r\n");
    const auto lf_index = content.find('\n');
    if (lf_index == std::string_view::npos) {
        return "\n";
    }
    if (crlf_index == std::string_view::npos) {
        return "\n";
    }
    return crlf_index < lf_index ? "\r\n" : "\n";
}

std::string normalize_to_lf(std::string_view text) {
    std::string result;
    result.reserve(text.size());
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (text[index] == '\r') {
            if (index + 1 < text.size() && text[index + 1] == '\n') {
                result.push_back('\n');
                ++index;
            } else {
                result.push_back('\n');
            }
        } else {
            result.push_back(text[index]);
        }
    }
    return result;
}

std::string restore_line_endings(std::string_view text, std::string_view ending) {
    if (ending != "\r\n") {
        return std::string(text);
    }
    std::string result;
    result.reserve(text.size());
    for (const char character : text) {
        if (character == '\n') {
            result += "\r\n";
        } else {
            result.push_back(character);
        }
    }
    return result;
}

std::pair<std::string, std::string> strip_bom(std::string_view content) {
    if (content.starts_with("\xef\xbb\xbf")) {
        return {std::string("\xef\xbb\xbf"), std::string(content.substr(3))};
    }
    return {{}, std::string(content)};
}

support::Expected<AppliedEdits> apply_edits_to_normalized_content(
    std::string_view normalized_content,
    const std::vector<EditReplacement>& edits,
    std::string_view path) {
    std::vector<EditReplacement> normalized_edits;
    normalized_edits.reserve(edits.size());
    for (const auto& edit : edits) {
        normalized_edits.push_back(EditReplacement{
            .old_text = normalize_to_lf(edit.old_text),
            .new_text = normalize_to_lf(edit.new_text),
        });
    }

    for (std::size_t index = 0; index < normalized_edits.size(); ++index) {
        if (normalized_edits[index].old_text.empty()) {
            if (normalized_edits.size() == 1) {
                return std::unexpected(edit_error(
                    "oldText must not be empty in " + std::string(path) + "."));
            }
            return std::unexpected(edit_error(
                "edits[" + std::to_string(index) +
                "].oldText must not be empty in " + std::string(path) + "."));
        }
    }

    std::vector<FuzzyMatchResult> initial_matches;
    initial_matches.reserve(normalized_edits.size());
    for (const auto& edit : normalized_edits) {
        initial_matches.push_back(fuzzy_find_text(normalized_content, edit.old_text));
    }
    const bool used_fuzzy_match = std::any_of(
        initial_matches.begin(), initial_matches.end(),
        [](const FuzzyMatchResult& match) { return match.used_fuzzy_match; });
    const auto replacement_base_content =
        used_fuzzy_match
            ? normalize_for_fuzzy_match(normalized_content)
            : std::string(normalized_content);

    std::vector<MatchedEdit> matched_edits;
    matched_edits.reserve(normalized_edits.size());
    for (std::size_t index = 0; index < normalized_edits.size(); ++index) {
        const auto& edit = normalized_edits[index];
        const auto match_result = fuzzy_find_text(replacement_base_content, edit.old_text);
        if (!match_result.found) {
            if (normalized_edits.size() == 1) {
                return std::unexpected(edit_error(
                    "Could not find the exact text in " + std::string(path) +
                    ". The old text must match exactly including all whitespace and newlines."));
            }
            return std::unexpected(edit_error(
                "Could not find edits[" + std::to_string(index) +
                "] in " + std::string(path) +
                ". The oldText must match exactly including all whitespace and newlines."));
        }
        const auto occurrences = count_occurrences(replacement_base_content, edit.old_text);
        if (occurrences > 1) {
            if (normalized_edits.size() == 1) {
                return std::unexpected(edit_error(
                    "Found " + std::to_string(occurrences) +
                    " occurrences of the text in " + std::string(path) +
                    ". The text must be unique. Please provide more context to make it unique."));
            }
            return std::unexpected(edit_error(
                "Found " + std::to_string(occurrences) +
                " occurrences of edits[" + std::to_string(index) +
                "] in " + std::string(path) +
                ". Each oldText must be unique. Please provide more context to make it unique."));
        }
        matched_edits.push_back(MatchedEdit{
            .edit_index = index,
            .match_index = match_result.index,
            .match_length = match_result.match_length,
            .new_text = edit.new_text,
        });
    }

    std::stable_sort(matched_edits.begin(), matched_edits.end(), [](const MatchedEdit& left, const MatchedEdit& right) {
        return left.match_index < right.match_index;
    });
    for (std::size_t index = 1; index < matched_edits.size(); ++index) {
        const auto& previous = matched_edits[index - 1];
        const auto& current = matched_edits[index];
        if (previous.match_index + previous.match_length > current.match_index) {
            return std::unexpected(edit_error(
                "edits[" + std::to_string(previous.edit_index) +
                "] and edits[" + std::to_string(current.edit_index) +
                "] overlap in " + std::string(path) +
                ". Merge them into one edit or target disjoint regions."));
        }
    }

    const std::string base_content(normalized_content);
    std::vector<TextReplacement> replacements;
    replacements.reserve(matched_edits.size());
    for (const auto& matched : matched_edits) {
        replacements.push_back(TextReplacement{
            .match_index = matched.match_index,
            .match_length = matched.match_length,
            .new_text = matched.new_text,
        });
    }
    support::Expected<std::string> new_content;
    if (used_fuzzy_match) {
        auto overlaid = apply_replacements_preserving_unchanged_lines(
            base_content, replacement_base_content, replacements);
        if (!overlaid) {
            return std::unexpected(edit_error(
                "Cannot preserve unchanged lines because the base content has a different line count."));
        }
        new_content = std::move(*overlaid);
    } else {
        new_content = apply_replacements(replacement_base_content, replacements);
    }
    if (!new_content) {
        return std::unexpected(new_content.error());
    }

    if (base_content == *new_content) {
        if (normalized_edits.size() == 1) {
            return std::unexpected(edit_error(
                "No changes made to " + std::string(path) +
                ". The replacement produced identical content. This might indicate an issue with special characters or the text not existing as expected."));
        }
        return std::unexpected(edit_error(
            "No changes made to " + std::string(path) +
            ". The replacements produced identical content."));
    }

    return AppliedEdits{
        .base_content = std::move(base_content),
        .new_content = std::move(*new_content),
    };
}

DisplayDiff generate_diff_string(
    std::string_view old_content,
    std::string_view new_content,
    std::size_t context_lines) {
    const auto parts = diff_lines(old_content, new_content);

    const auto old_lines = split_lines_with_endings(old_content);
    const auto new_lines = split_lines_with_endings(new_content);
    const auto max_line_num = std::max(old_lines.size(), new_lines.size());
    const auto line_num_width =
        std::max<std::size_t>(1, std::to_string(max_line_num).size());

    auto pad_line_num = [&](std::size_t line_num) {
        return std::format("{:>{}}", line_num, line_num_width);
    };

    std::string output;
    std::size_t old_line_num = 1;
    std::size_t new_line_num = 1;
    bool last_was_change = false;
    std::optional<int> first_changed_line;

    for (std::size_t index = 0; index < parts.size(); ++index) {
        const auto& part = parts[index];
        std::vector<std::string_view> raw;
        raw.reserve(part.lines.size());
        for (const auto& line : part.lines) {
            auto view = std::string_view(line);
            if (!view.empty() && view.back() == '\n') {
                view.remove_suffix(1);
            }
            raw.push_back(view);
        }

        if (part.kind == DiffPartKind::Added || part.kind == DiffPartKind::Removed) {
            if (!first_changed_line) {
                first_changed_line = static_cast<int>(new_line_num);
            }
            for (const auto& line : raw) {
                if (part.kind == DiffPartKind::Added) {
                    output += "+" + pad_line_num(new_line_num) + " " +
                              std::string(line) + "\n";
                    ++new_line_num;
                } else {
                    output += "-" + pad_line_num(old_line_num) + " " +
                              std::string(line) + "\n";
                    ++old_line_num;
                }
            }
            last_was_change = true;
        } else {
            const bool next_part_is_change =
                index + 1 < parts.size() &&
                (parts[index + 1].kind == DiffPartKind::Added ||
                 parts[index + 1].kind == DiffPartKind::Removed);
            const bool has_leading_change = last_was_change;
            const bool has_trailing_change = next_part_is_change;

            auto emit_context_line = [&](std::string_view line) {
                output += " " + pad_line_num(old_line_num) + " " +
                          std::string(line) + "\n";
                ++old_line_num;
                ++new_line_num;
            };
            auto emit_ellipsis = [&](std::size_t skipped) {
                output += " " + std::string(line_num_width, ' ') + " ...\n";
                old_line_num += skipped;
                new_line_num += skipped;
            };

            if (has_leading_change && has_trailing_change) {
                if (raw.size() <= context_lines * 2) {
                    for (const auto& line : raw) {
                        emit_context_line(line);
                    }
                } else {
                    for (std::size_t line_index = 0; line_index < context_lines; ++line_index) {
                        emit_context_line(raw[line_index]);
                    }
                    const auto skipped =
                        raw.size() - context_lines * 2;
                    emit_ellipsis(skipped);
                    for (std::size_t line_index = raw.size() - context_lines;
                         line_index < raw.size();
                         ++line_index) {
                        emit_context_line(raw[line_index]);
                    }
                }
            } else if (has_leading_change) {
                const auto shown = std::min(raw.size(), context_lines);
                for (std::size_t line_index = 0; line_index < shown; ++line_index) {
                    emit_context_line(raw[line_index]);
                }
                const auto skipped = raw.size() - shown;
                if (skipped > 0) {
                    emit_ellipsis(skipped);
                }
            } else if (has_trailing_change) {
                const auto skipped = raw.size() > context_lines
                                         ? raw.size() - context_lines
                                         : 0;
                if (skipped > 0) {
                    emit_ellipsis(skipped);
                }
                for (std::size_t line_index = skipped; line_index < raw.size(); ++line_index) {
                    emit_context_line(raw[line_index]);
                }
            } else {
                old_line_num += raw.size();
                new_line_num += raw.size();
            }
            last_was_change = false;
        }
    }

    return DisplayDiff{
        .diff = std::move(output),
        .first_changed_line = first_changed_line,
    };
}

std::string generate_unified_patch(
    std::string_view path,
    std::string_view old_content,
    std::string_view new_content,
    std::size_t context_lines) {
    // jsdiff structuredPatch: walk diffLines parts (lines keep their trailing
    // newline) plus an empty closing part, then strip trailing newlines and
    // add "\ No newline at end of file" markers.
    struct Hunk {
        std::size_t old_start{0};
        std::size_t old_lines{0};
        std::size_t new_start{0};
        std::size_t new_lines{0};
        std::vector<std::string> lines;
    };

    auto parts = diff_lines(old_content, new_content);
    parts.push_back(DiffPart{DiffPartKind::Common, {}});

    std::vector<Hunk> hunks;
    std::size_t old_range_start = 0;
    std::size_t new_range_start = 0;
    std::vector<std::string> cur_range;
    std::size_t old_line = 1;
    std::size_t new_line = 1;

    auto context_lines_for = [&](const std::vector<std::string>& lines) {
        std::vector<std::string> result;
        const auto start = lines.size() > context_lines
                               ? lines.size() - context_lines
                               : 0;
        for (auto index = start; index < lines.size(); ++index) {
            result.push_back(" " + lines[index]);
        }
        return result;
    };

    for (std::size_t index = 0; index < parts.size(); ++index) {
        const auto& current = parts[index];
        const auto& lines = current.lines;
        if (current.kind == DiffPartKind::Added ||
            current.kind == DiffPartKind::Removed) {
            if (old_range_start == 0) {
                old_range_start = old_line;
                new_range_start = new_line;
                if (index > 0) {
                    cur_range = context_lines_for(parts[index - 1].lines);
                    old_range_start -= cur_range.size();
                    new_range_start -= cur_range.size();
                }
            }
            for (const auto& line : lines) {
                cur_range.push_back(
                    (current.kind == DiffPartKind::Added ? "+" : "-") + line);
            }
            if (current.kind == DiffPartKind::Added) {
                new_line += lines.size();
            } else {
                old_line += lines.size();
            }
        } else {
            if (old_range_start != 0) {
                if (lines.size() <= context_lines * 2 && index + 1 < parts.size() - 1) {
                    for (const auto& line : lines) {
                        cur_range.push_back(" " + line);
                    }
                } else {
                    const auto context_size = std::min(lines.size(), context_lines);
                    for (std::size_t line_index = 0; line_index < context_size; ++line_index) {
                        cur_range.push_back(" " + lines[line_index]);
                    }
                    hunks.push_back(Hunk{
                        .old_start = old_range_start,
                        .old_lines = old_line - old_range_start + context_size,
                        .new_start = new_range_start,
                        .new_lines = new_line - new_range_start + context_size,
                        .lines = std::move(cur_range),
                    });
                    cur_range = {};
                    old_range_start = 0;
                    new_range_start = 0;
                }
            }
            old_line += lines.size();
            new_line += lines.size();
        }
    }

    std::string patch;
    patch += "--- " + std::string(path) + "\n";
    patch += "+++ " + std::string(path) + "\n";
    for (auto& hunk : hunks) {
        if (hunk.old_lines == 0) {
            --hunk.old_start;
        }
        if (hunk.new_lines == 0) {
            --hunk.new_start;
        }
        patch += "@@ -" + std::to_string(hunk.old_start) + "," +
                 std::to_string(hunk.old_lines) + " +" +
                 std::to_string(hunk.new_start) + "," +
                 std::to_string(hunk.new_lines) + " @@\n";
        std::vector<std::string> rendered;
        rendered.reserve(hunk.lines.size() + 1);
        for (auto& line : hunk.lines) {
            if (!line.empty() && line.back() == '\n') {
                line.pop_back();
                rendered.push_back(std::move(line));
            } else {
                rendered.push_back(std::move(line));
                rendered.push_back("\\ No newline at end of file");
            }
        }
        for (const auto& line : rendered) {
            patch += line + "\n";
        }
    }
    return patch;
}

} // namespace cch::tools

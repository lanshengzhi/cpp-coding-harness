#include "EditDiff.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::durable {
namespace {

[[nodiscard]] support::Error edit_error(std::string message) {
    return support::make_error(support::ErrorCode::Tool, std::move(message));
}

/// Frozen pi `splitLinesWithEndings` and jsdiff's line tokenizer: every line
/// keeps its trailing newline and the last line may have none.
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

/// The line number the frozen formatter's gutter must fit: JavaScript
/// `content.split("\n").length`, which counts one more line for a trailing
/// newline than the file has.
[[nodiscard]] std::size_t frozen_line_count(std::string_view content) {
    return static_cast<std::size_t>(std::ranges::count(content, '\n')) + 1;
}

// ---------------------------------------------------------------------------
// Line-based diff: jsdiff `diffLines` Myers walk and component list
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

/// One change component of the greedy Myers walk (jsdiff `Diff` component). A
/// component list is persistent, so two surviving paths can share a tail; nodes
/// live in one pool and are addressed by index.
struct MyersComponent {
    std::uint32_t count{0};
    std::uint32_t previous{0};
    DiffPartKind kind{DiffPartKind::Common};
};

inline constexpr std::uint32_t kNoComponent = static_cast<std::uint32_t>(-1);

struct MyersPath {
    std::ptrdiff_t old_position{-1};
    std::uint32_t last{kNoComponent};
};

/// The walk is bounded so a pathological rewrite cannot spend unbounded time or
/// memory: beyond either bound the caller keeps a coarse whole-region
/// replacement instead of a minimal line diff.
// debt: a bounded greedy Myers window can show one coarser hunk where frozen pi shows a minimal diff,
// upgrade to a linear-space middle-snake walk if a real edit exceeds the window.
inline constexpr std::ptrdiff_t kMaxEditLength = 2048;
inline constexpr std::size_t kMaxComponents = 1U << 21;

/// jsdiff `Diff.diff` over line tokens: a greedy forward Myers walk with
/// jsdiff's path-selection rule, so repeated lines resolve to the same diff.
/// Returns nullopt when a bound is exceeded.
[[nodiscard]] std::optional<std::vector<DiffPart>> myers_diff(
        const std::vector<std::string>& old_tokens, const std::vector<std::string>& new_tokens) {
    const auto old_length = static_cast<std::ptrdiff_t>(old_tokens.size());
    const auto new_length = static_cast<std::ptrdiff_t>(new_tokens.size());

    std::vector<MyersComponent> components;
    auto add_component =
            [&](std::uint32_t count, DiffPartKind kind, std::uint32_t previous) -> std::optional<std::uint32_t> {
        if (components.size() >= kMaxComponents) {
            return std::nullopt;
        }
        components.push_back(MyersComponent{.count = count, .previous = previous, .kind = kind});
        return static_cast<std::uint32_t>(components.size() - 1);
    };

    // A path is kept per diagonal; the index is diagonal + kMaxEditLength + 1.
    const auto slot = [](std::ptrdiff_t diagonal) { return static_cast<std::size_t>(diagonal + kMaxEditLength + 1); };
    std::vector<MyersPath> best_path(static_cast<std::size_t>(2 * kMaxEditLength + 3));
    std::vector<bool> present(best_path.size(), false);

    auto extract_common = [&](MyersPath& path, std::ptrdiff_t diagonal) {
        auto old_position = path.old_position;
        auto new_position = old_position - diagonal;
        std::uint32_t common = 0;
        while (new_position + 1 < new_length && old_position + 1 < old_length &&
                old_tokens[static_cast<std::size_t>(old_position + 1)] ==
                        new_tokens[static_cast<std::size_t>(new_position + 1)]) {
            ++new_position;
            ++old_position;
            ++common;
        }
        if (common > 0) {
            auto node = add_component(common, DiffPartKind::Common, path.last);
            if (!node) {
                return std::optional<std::ptrdiff_t>{};
            }
            path.last = *node;
        }
        path.old_position = old_position;
        return std::optional<std::ptrdiff_t>{new_position};
    };

    auto add_to_path = [&](const MyersPath& path, DiffPartKind kind, std::ptrdiff_t old_increment) {
        std::uint32_t count = 1;
        std::uint32_t previous = path.last;
        if (previous != kNoComponent && components[previous].kind == kind) {
            count = components[previous].count + 1;
            previous = components[previous].previous;
        }
        auto node = add_component(count, kind, previous);
        if (!node) {
            return std::optional<MyersPath>{};
        }
        return std::optional<MyersPath>{MyersPath{.old_position = path.old_position + old_increment, .last = *node}};
    };

    auto build_parts = [&](std::uint32_t last) {
        std::vector<std::uint32_t> order;
        for (auto node = last; node != kNoComponent; node = components[node].previous) {
            order.push_back(node);
        }
        std::ranges::reverse(order);
        std::vector<DiffPart> parts;
        std::size_t old_position = 0;
        std::size_t new_position = 0;
        for (const auto node : order) {
            const auto& component = components[node];
            const auto& source = component.kind == DiffPartKind::Removed ? old_tokens : new_tokens;
            auto& position = component.kind == DiffPartKind::Removed ? old_position : new_position;
            DiffPart part{.kind = component.kind, .lines = {}};
            part.lines.reserve(component.count);
            for (std::uint32_t index = 0; index < component.count; ++index) {
                part.lines.push_back(source[position + index]);
            }
            position += component.count;
            if (component.kind == DiffPartKind::Common) {
                old_position += component.count;
            }
            parts.push_back(std::move(part));
        }
        return parts;
    };

    MyersPath start;
    auto new_position = extract_common(start, 0);
    if (!new_position) {
        return std::nullopt;
    }
    if (start.old_position + 1 >= old_length && *new_position + 1 >= new_length) {
        return build_parts(start.last);
    }
    present[slot(0)] = true;
    best_path[slot(0)] = start;

    std::ptrdiff_t min_diagonal = -kMaxEditLength - 1;
    std::ptrdiff_t max_diagonal = kMaxEditLength + 1;
    for (std::ptrdiff_t edit_length = 1; edit_length <= kMaxEditLength; ++edit_length) {
        for (std::ptrdiff_t diagonal = std::max(min_diagonal, -edit_length);
                diagonal <= std::min(max_diagonal, edit_length);
                diagonal += 2) {
            const auto remove_slot = slot(diagonal - 1);
            const auto add_slot = slot(diagonal + 1);
            const bool has_remove = present[remove_slot];
            const bool has_add = present[add_slot];
            MyersPath remove_path = best_path[remove_slot];
            MyersPath add_path = best_path[add_slot];
            if (has_remove) {
                present[remove_slot] = false;
            }
            bool can_add = false;
            if (has_add) {
                const auto after_insert = add_path.old_position - diagonal;
                can_add = after_insert >= 0 && after_insert < new_length;
            }
            const bool can_remove = has_remove && remove_path.old_position + 1 < old_length;
            if (!can_add && !can_remove) {
                present[slot(diagonal)] = false;
                continue;
            }
            std::optional<MyersPath> base;
            if (!can_remove || (can_add && remove_path.old_position < add_path.old_position)) {
                base = add_to_path(add_path, DiffPartKind::Added, 0);
            } else {
                base = add_to_path(remove_path, DiffPartKind::Removed, 1);
            }
            if (!base) {
                return std::nullopt;
            }
            auto after_common = extract_common(*base, diagonal);
            if (!after_common) {
                return std::nullopt;
            }
            if (base->old_position + 1 >= old_length && *after_common + 1 >= new_length) {
                return build_parts(base->last);
            }
            best_path[slot(diagonal)] = *base;
            present[slot(diagonal)] = true;
            if (base->old_position + 1 >= old_length) {
                max_diagonal = std::min(max_diagonal, diagonal - 1);
            }
            if (*after_common + 1 >= new_length) {
                min_diagonal = std::max(min_diagonal, diagonal + 1);
            }
        }
    }
    return std::nullopt;
}

/// jsdiff `diffLines` parts. The bounded walk above is exact for ordinary
/// edits; a diff that exceeds its bound falls back to the common prefix and
/// suffix around one whole-region replacement.
[[nodiscard]] std::vector<DiffPart> diff_lines(std::string_view old_content, std::string_view new_content) {
    const auto old_lines = split_lines_with_endings(old_content);
    const auto new_lines = split_lines_with_endings(new_content);
    if (auto parts = myers_diff(old_lines, new_lines)) {
        return std::move(*parts);
    }

    std::size_t prefix = 0;
    while (prefix < old_lines.size() && prefix < new_lines.size() && old_lines[prefix] == new_lines[prefix]) {
        ++prefix;
    }
    std::size_t suffix_old = old_lines.size();
    std::size_t suffix_new = new_lines.size();
    while (suffix_old > prefix && suffix_new > prefix && old_lines[suffix_old - 1] == new_lines[suffix_new - 1]) {
        --suffix_old;
        --suffix_new;
    }
    std::vector<DiffPart> parts;
    auto push = [&](DiffPartKind kind,
                        std::vector<std::string>::const_iterator first,
                        std::vector<std::string>::const_iterator last) {
        if (first == last) {
            return;
        }
        parts.push_back(DiffPart{.kind = kind, .lines = {first, last}});
    };
    push(DiffPartKind::Common, old_lines.begin(), old_lines.begin() + static_cast<std::ptrdiff_t>(prefix));
    push(DiffPartKind::Removed,
            old_lines.begin() + static_cast<std::ptrdiff_t>(prefix),
            old_lines.begin() + static_cast<std::ptrdiff_t>(suffix_old));
    push(DiffPartKind::Added,
            new_lines.begin() + static_cast<std::ptrdiff_t>(prefix),
            new_lines.begin() + static_cast<std::ptrdiff_t>(suffix_new));
    push(DiffPartKind::Common, old_lines.begin() + static_cast<std::ptrdiff_t>(suffix_old), old_lines.end());
    return parts;
}

// ---------------------------------------------------------------------------
// Exact-text replacement (frozen pi edit-diff.ts applyEditsToNormalizedContent)
// ---------------------------------------------------------------------------

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

/// Frozen pi `applyReplacements`: applied in reverse order so offsets stay
/// stable.
[[nodiscard]] std::string apply_replacements(
        std::string_view content, const std::vector<TextReplacement>& replacements) {
    std::string result(content);
    for (auto it = replacements.rbegin(); it != replacements.rend(); ++it) {
        result.replace(it->match_index, it->match_length, it->new_text);
    }
    return result;
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
                ++index;
            }
            result.push_back('\n');
            continue;
        }
        result.push_back(text[index]);
    }
    return result;
}

std::string restore_line_endings(std::string_view text, std::string_view ending) {
    if (ending != "\r\n") {
        return std::string(text);
    }
    std::string result;
    result.reserve(text.size() + text.size() / 8);
    for (const char character : text) {
        if (character == '\n') {
            result.append("\r\n");
            continue;
        }
        result.push_back(character);
    }
    return result;
}

StrippedContent strip_bom(std::string_view content) {
    if (content.starts_with("\xef\xbb\xbf")) {
        return StrippedContent{.bom = std::string("\xef\xbb\xbf"), .text = std::string(content.substr(3))};
    }
    return StrippedContent{.bom = {}, .text = std::string(content)};
}

support::Expected<AppliedEdits> apply_edits_to_normalized_content(
        std::string_view normalized_content, const std::vector<EditReplacement>& edits, std::string_view path) {
    std::vector<EditReplacement> normalized_edits;
    normalized_edits.reserve(edits.size());
    for (const auto& edit : edits) {
        normalized_edits.push_back(EditReplacement{
                .old_text = normalize_to_lf(edit.old_text),
                .new_text = normalize_to_lf(edit.new_text),
        });
    }

    for (std::size_t index = 0; index < normalized_edits.size(); ++index) {
        if (!normalized_edits[index].old_text.empty()) {
            continue;
        }
        if (normalized_edits.size() == 1) {
            return std::unexpected(edit_error(std::format("oldText must not be empty in {}.", path)));
        }
        return std::unexpected(edit_error(std::format("edits[{}].oldText must not be empty in {}.", index, path)));
    }

    std::vector<MatchedEdit> matched;
    matched.reserve(normalized_edits.size());
    for (std::size_t index = 0; index < normalized_edits.size(); ++index) {
        const auto& edit = normalized_edits[index];
        const auto found = normalized_content.find(edit.old_text);
        if (found == std::string_view::npos) {
            if (normalized_edits.size() == 1) {
                return std::unexpected(edit_error(
                        std::format("Could not find the exact text in {}. The old text must match exactly including "
                                    "all whitespace and newlines.",
                                path)));
            }
            return std::unexpected(edit_error(
                    std::format("Could not find edits[{}] in {}. The oldText must match exactly including all "
                                "whitespace and newlines.",
                            index,
                            path)));
        }
        // Frozen pi counts non-overlapping occurrences (`split`), not every
        // starting offset.
        std::size_t occurrences = 0;
        for (auto at = found; at != std::string_view::npos;
                at = normalized_content.find(edit.old_text, at + edit.old_text.size())) {
            ++occurrences;
        }
        if (occurrences > 1) {
            if (normalized_edits.size() == 1) {
                return std::unexpected(edit_error(
                        std::format("Found {} occurrences of the text in {}. The text must be unique. Please provide "
                                    "more context to make it unique.",
                                occurrences,
                                path)));
            }
            return std::unexpected(edit_error(
                    std::format("Found {} occurrences of edits[{}] in {}. Each oldText must be unique. Please provide "
                                "more context to make it unique.",
                            occurrences,
                            index,
                            path)));
        }
        matched.push_back(MatchedEdit{
                .edit_index = index,
                .match_index = found,
                .match_length = edit.old_text.size(),
                .new_text = edit.new_text,
        });
    }

    std::stable_sort(matched.begin(), matched.end(), [](const MatchedEdit& left, const MatchedEdit& right) {
        return left.match_index < right.match_index;
    });
    for (std::size_t index = 1; index < matched.size(); ++index) {
        const auto& previous = matched[index - 1];
        const auto& current = matched[index];
        if (previous.match_index + previous.match_length > current.match_index) {
            return std::unexpected(edit_error(
                    std::format("edits[{}] and edits[{}] overlap in {}. Merge them into one edit or target disjoint "
                                "regions.",
                            previous.edit_index,
                            current.edit_index,
                            path)));
        }
    }

    std::vector<TextReplacement> ordered;
    ordered.reserve(matched.size());
    for (const auto& edit : matched) {
        ordered.push_back(TextReplacement{
                .match_index = edit.match_index,
                .match_length = edit.match_length,
                .new_text = edit.new_text,
        });
    }
    auto new_content = apply_replacements(normalized_content, ordered);
    const std::string base_content(normalized_content);
    if (base_content == new_content) {
        if (normalized_edits.size() == 1) {
            return std::unexpected(edit_error(
                    std::format("No changes made to {}. The replacement produced identical content. This might "
                                "indicate an issue with special characters or the text not existing as expected.",
                            path)));
        }
        return std::unexpected(
                edit_error(std::format("No changes made to {}. The replacements produced identical content.", path)));
    }
    return AppliedEdits{
            .base_content = std::move(base_content),
            .new_content = std::move(new_content),
    };
}

DisplayDiff generate_diff_string(
        std::string_view old_content, std::string_view new_content, std::size_t context_lines) {
    const auto parts = diff_lines(old_content, new_content);

    const auto max_line_num = std::max(frozen_line_count(old_content), frozen_line_count(new_content));
    const auto line_num_width = std::to_string(max_line_num).size();
    auto pad_line_num = [&](std::size_t line_num) { return std::format("{:>{}}", line_num, line_num_width); };

    std::vector<std::string> output;
    std::size_t old_line_num = 1;
    std::size_t new_line_num = 1;
    bool last_was_change = false;
    // Frozen pi keeps the first changed line as a JavaScript number; the detail
    // stays absent for a line count the reported width cannot hold.
    std::optional<int> first_changed_line;

    auto emit = [&](std::string line) { output.push_back(std::move(line)); };
    auto emit_context_line = [&](std::string_view line) {
        emit(" " + pad_line_num(old_line_num) + " " + std::string(line));
        ++old_line_num;
        ++new_line_num;
    };
    auto emit_ellipsis = [&](std::size_t skipped) {
        emit(" " + std::string(line_num_width, ' ') + " ...");
        old_line_num += skipped;
        new_line_num += skipped;
    };

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
            if (!first_changed_line && std::in_range<int>(new_line_num)) {
                first_changed_line = static_cast<int>(new_line_num);
            }
            for (const auto& line : raw) {
                if (part.kind == DiffPartKind::Added) {
                    emit("+" + pad_line_num(new_line_num) + " " + std::string(line));
                    ++new_line_num;
                } else {
                    emit("-" + pad_line_num(old_line_num) + " " + std::string(line));
                    ++old_line_num;
                }
            }
            last_was_change = true;
            continue;
        }

        const bool next_part_is_change =
                index + 1 < parts.size() &&
                (parts[index + 1].kind == DiffPartKind::Added || parts[index + 1].kind == DiffPartKind::Removed);
        const bool has_leading_change = last_was_change;
        const bool has_trailing_change = next_part_is_change;

        if (has_leading_change && has_trailing_change) {
            if (raw.size() <= context_lines * 2) {
                for (const auto& line : raw) {
                    emit_context_line(line);
                }
            } else {
                for (std::size_t line_index = 0; line_index < context_lines; ++line_index) {
                    emit_context_line(raw[line_index]);
                }
                emit_ellipsis(raw.size() - context_lines * 2);
                for (std::size_t line_index = raw.size() - context_lines; line_index < raw.size(); ++line_index) {
                    emit_context_line(raw[line_index]);
                }
            }
        } else if (has_leading_change) {
            const auto shown = std::min(raw.size(), context_lines);
            for (std::size_t line_index = 0; line_index < shown; ++line_index) {
                emit_context_line(raw[line_index]);
            }
            if (shown < raw.size()) {
                emit_ellipsis(raw.size() - shown);
            }
        } else if (has_trailing_change) {
            const auto skipped = raw.size() > context_lines ? raw.size() - context_lines : 0;
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

    std::string diff;
    for (const auto& line : output) {
        if (!diff.empty()) {
            diff.push_back('\n');
        }
        diff += line;
    }
    return DisplayDiff{
            .diff = std::move(diff),
            .first_changed_line = first_changed_line,
    };
}

std::string generate_unified_patch(
        std::string_view path, std::string_view old_content, std::string_view new_content, std::size_t context_lines) {
    // jsdiff `structuredPatch` walks the diffLines parts plus an empty closing
    // part, keeps the trailing newlines the parts carry, and marks a final line
    // without one.
    struct Hunk {
        std::size_t old_start{0};
        std::size_t old_lines{0};
        std::size_t new_start{0};
        std::size_t new_lines{0};
        std::vector<std::string> lines;
    };

    auto parts = diff_lines(old_content, new_content);
    parts.push_back(DiffPart{.kind = DiffPartKind::Common, .lines = {}});

    std::vector<Hunk> hunks;
    std::size_t old_range_start = 0;
    std::size_t new_range_start = 0;
    std::vector<std::string> cur_range;
    std::size_t old_line = 1;
    std::size_t new_line = 1;

    auto context_lines_for = [&](const std::vector<std::string>& lines) {
        std::vector<std::string> result;
        const auto start = lines.size() > context_lines ? lines.size() - context_lines : 0;
        for (auto index = start; index < lines.size(); ++index) {
            result.push_back(" " + lines[index]);
        }
        return result;
    };

    for (std::size_t index = 0; index < parts.size(); ++index) {
        const auto& current = parts[index];
        const auto& lines = current.lines;
        if (current.kind == DiffPartKind::Added || current.kind == DiffPartKind::Removed) {
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
                cur_range.push_back((current.kind == DiffPartKind::Added ? "+" : "-") + line);
            }
            if (current.kind == DiffPartKind::Added) {
                new_line += lines.size();
            } else {
                old_line += lines.size();
            }
            continue;
        }
        if (old_range_start == 0) {
            old_line += lines.size();
            new_line += lines.size();
            continue;
        }
        // A trailing common run either closes the hunk after a full context
        // window or stays inside it when the hunks would otherwise merge.
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
        old_line += lines.size();
        new_line += lines.size();
    }

    std::string patch = std::format("--- {0}\n+++ {0}\n", path);
    for (auto& hunk : hunks) {
        if (hunk.old_lines == 0) {
            --hunk.old_start;
        }
        if (hunk.new_lines == 0) {
            --hunk.new_start;
        }
        patch += std::format("@@ -{},{} +{},{} @@\n", hunk.old_start, hunk.old_lines, hunk.new_start, hunk.new_lines);
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

} // namespace cch::durable

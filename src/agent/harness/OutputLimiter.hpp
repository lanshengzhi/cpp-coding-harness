#pragma once

#include "support/BoundedText.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <format>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::harness {

struct OutputLimit {
    std::size_t max_bytes{50 * 1024};
    std::size_t max_lines{2000};
};

struct OutputLimitResult {
    std::string text;
    bool truncated{false};
};

/// Which of the two independent limits a truncation hit (pi
/// `core/tools/truncate.ts` `TruncationResult.truncatedBy`).
enum class OutputTruncationKind {
    Lines,
    Bytes,
};

/// The facts one truncation step produces, mirroring pi's `TruncationResult`
/// (`core/tools/truncate.ts:15-38`) minus `content`: the retained text stays
/// the model-facing `content` the caller returns, and every other field travels
/// to the tool result as structured `details.truncation`. The tool layer reads
/// these fields instead of recomputing them at the call site.
struct OutputTruncation {
    std::string text;
    bool truncated{false};
    std::optional<OutputTruncationKind> truncated_by{std::nullopt};
    std::size_t total_lines{0};
    std::size_t total_bytes{0};
    std::size_t output_lines{0};
    std::size_t output_bytes{0};
    bool last_line_partial{false};
    bool first_line_exceeds_limit{false};
    std::size_t max_lines{0};
    std::size_t max_bytes{0};
};

/// pi `String.prototype.split("\n")`: a trailing newline yields a trailing
/// empty line, and the empty string is one empty line.
[[nodiscard]] inline std::vector<std::string_view> split_lines(std::string_view text) {
    std::vector<std::string_view> lines;
    std::size_t start = 0;
    while (true) {
        const auto newline = text.find('\n', start);
        if (newline == std::string_view::npos) {
            lines.push_back(text.substr(start));
            return lines;
        }
        lines.push_back(text.substr(start, newline - start));
        start = newline + 1;
    }
}

/// pi `splitLinesForCounting` (`truncate.ts:47`): the same split without the
/// trailing empty element, and no lines at all for the empty string.
[[nodiscard]] inline std::vector<std::string_view> split_lines_for_counting(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    auto lines = split_lines(text);
    if (text.back() == '\n') {
        lines.pop_back();
    }
    return lines;
}

/// pi `truncateHead` (`truncate.ts:71-160`): keep the first lines that fit, never
/// a partial line, and report an empty text with `first_line_exceeds_limit` when
/// the very first line alone is over the byte budget.
[[nodiscard]] inline OutputTruncation truncate_output_head(std::string_view content, OutputLimit limit = {}) {
    const auto lines = split_lines_for_counting(content);
    OutputTruncation result{
            .text = std::string(content),
            .truncated = false,
            .truncated_by = std::nullopt,
            .total_lines = lines.size(),
            .total_bytes = content.size(),
            .output_lines = lines.size(),
            .output_bytes = content.size(),
            .last_line_partial = false,
            .first_line_exceeds_limit = false,
            .max_lines = limit.max_lines,
            .max_bytes = limit.max_bytes,
    };
    if (result.total_lines <= limit.max_lines && result.total_bytes <= limit.max_bytes) {
        return result;
    }
    if (lines.front().size() > limit.max_bytes) {
        result.text.clear();
        result.truncated = true;
        result.truncated_by = OutputTruncationKind::Bytes;
        result.output_lines = 0;
        result.output_bytes = 0;
        result.first_line_exceeds_limit = true;
        return result;
    }

    std::string kept;
    std::size_t kept_bytes = 0;
    std::size_t kept_lines = 0;
    auto truncated_by = OutputTruncationKind::Lines;
    for (std::size_t index = 0; index < lines.size() && index < limit.max_lines; ++index) {
        // pi counts the newline that joins a line to the previous one.
        const auto line_bytes = lines[index].size() + (index > 0 ? 1 : 0);
        if (kept_bytes + line_bytes > limit.max_bytes) {
            truncated_by = OutputTruncationKind::Bytes;
            break;
        }
        if (index > 0) {
            kept += '\n';
        }
        kept.append(lines[index]);
        kept_bytes += line_bytes;
        ++kept_lines;
    }

    result.text = std::move(kept);
    result.truncated = true;
    result.truncated_by = truncated_by;
    result.output_lines = kept_lines;
    result.output_bytes = result.text.size();
    return result;
}

/// pi `truncateTail` (`truncate.ts:168-241`): keep the last lines that fit, and
/// keep the tail of the final line itself when that line alone is over budget.
[[nodiscard]] inline OutputTruncation truncate_output_tail(std::string_view content, OutputLimit limit = {}) {
    const auto lines = split_lines_for_counting(content);
    OutputTruncation result{
            .text = std::string(content),
            .truncated = false,
            .truncated_by = std::nullopt,
            .total_lines = lines.size(),
            .total_bytes = content.size(),
            .output_lines = lines.size(),
            .output_bytes = content.size(),
            .last_line_partial = false,
            .first_line_exceeds_limit = false,
            .max_lines = limit.max_lines,
            .max_bytes = limit.max_bytes,
    };
    if (result.total_lines <= limit.max_lines && result.total_bytes <= limit.max_bytes) {
        return result;
    }

    // Collected back to front, then reversed into output order.
    std::vector<std::string_view> kept;
    std::size_t kept_bytes = 0;
    bool last_line_partial = false;
    auto truncated_by = OutputTruncationKind::Lines;
    for (std::size_t index = lines.size(); index-- > 0 && kept.size() < limit.max_lines;) {
        const auto line_bytes = lines[index].size() + (kept.empty() ? 0 : 1);
        if (kept_bytes + line_bytes > limit.max_bytes) {
            truncated_by = OutputTruncationKind::Bytes;
            if (kept.empty()) {
                // The final line alone is over budget: keep its tail.
                const auto tail_start = support::utf8_tail_start(
                        lines[index], limit.max_bytes, std::numeric_limits<std::size_t>::max());
                kept.push_back(lines[index].substr(tail_start));
                kept_bytes = kept.front().size();
                last_line_partial = true;
            }
            break;
        }
        kept.push_back(lines[index]);
        kept_bytes += line_bytes;
    }
    std::reverse(kept.begin(), kept.end());

    std::string text;
    for (std::size_t index = 0; index < kept.size(); ++index) {
        if (index > 0) {
            text += '\n';
        }
        text.append(kept[index]);
    }

    result.text = std::move(text);
    result.truncated = true;
    result.truncated_by = truncated_by;
    result.output_lines = kept.size();
    result.output_bytes = result.text.size();
    result.last_line_partial = last_line_partial;
    return result;
}

/// pi `formatSize` (`truncate.ts:61-69`) with JavaScript `toFixed(1)` rounding,
/// which rounds a decimal half away from zero where `std::format` would round
/// it to even.
[[nodiscard]] inline std::string format_output_size_tenths(double value) {
    const auto tenths = static_cast<long long>(std::floor(value * 10.0 + 0.5));
    return std::format("{}.{}", tenths / 10, tenths % 10);
}

[[nodiscard]] inline std::string format_output_size(std::size_t bytes) {
    if (bytes < 1024) {
        return std::to_string(bytes) + "B";
    }
    if (bytes < 1024 * 1024) {
        return format_output_size_tenths(static_cast<double>(bytes) / 1024.0) + "KB";
    }
    return format_output_size_tenths(static_cast<double>(bytes) / (1024.0 * 1024.0)) + "MB";
}

[[nodiscard]] inline OutputLimitResult limit_output_tail(
    const std::string& input,
    OutputLimit limit = {}) {
    if (input.empty()) {
        return {};
    }
    if (limit.max_bytes == 0 || limit.max_lines == 0) {
        return OutputLimitResult{.text = {}, .truncated = true};
    }

    // The byte/line walk and UTF-8 boundary repair are policy-free support
    // mechanics. Redaction-marker repair remains local to this harness wrapper.
    auto start = support::utf8_tail_start(input, limit.max_bytes, limit.max_lines);
    const auto marker = input.rfind(support::kRedactionMarker, start);
    if (marker != std::string::npos && marker < start && marker + support::kRedactionMarker.size() > start) {
        start = marker + support::kRedactionMarker.size();
    }

    return OutputLimitResult{
            .text = support::bounded_utf8(std::string_view(input).substr(start), limit.max_bytes),
            .truncated = start > 0,
    };
}

[[nodiscard]] inline OutputLimitResult limit_output_tail_redacted(
    std::string input,
    OutputLimit limit = {}) {
    return limit_output_tail(support::redact_text(std::move(input)), limit);
}

} // namespace cch::harness

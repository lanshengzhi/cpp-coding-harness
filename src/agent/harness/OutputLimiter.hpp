#pragma once

#include "support/BoundedText.hpp"

#include <sstream>
#include <string>
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

[[nodiscard]] inline OutputLimitResult limit_output(const std::string& input, OutputLimit limit = {}) {
    OutputLimitResult result;
    std::size_t bytes = 0;
    std::size_t lines = 0;
    std::istringstream stream(input);
    std::string line;
    while (std::getline(stream, line)) {
        const std::size_t next_bytes = bytes + line.size() + 1;
        if (lines >= limit.max_lines || next_bytes > limit.max_bytes) {
            result.truncated = true;
            break;
        }
        result.text += line;
        result.text += '\n';
        bytes = next_bytes;
        ++lines;
    }
    if (result.text.empty() && !input.empty()) {
        // The first line alone exceeds the byte budget; bound it without
        // splitting a UTF-8 multibyte sequence.
        result.text = support::bounded_utf8(input, limit.max_bytes);
        result.truncated = input.size() > limit.max_bytes;
    } else if (!input.empty() && input.back() != '\n' && !result.truncated && !result.text.empty()) {
        result.text.pop_back();
    }
    if (result.truncated) {
        result.text += "\n[output truncated]";
    }
    return result;
}

} // namespace cch::harness

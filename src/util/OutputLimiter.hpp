#pragma once

#include "BoundedText.hpp"

#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace cch::util {

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

    std::size_t start = input.size();
    std::size_t bytes = 0;
    std::size_t lines = 1;
    while (start > 0 && bytes < limit.max_bytes) {
        const char ch = input[start - 1];
        if (ch == '\n' && lines >= limit.max_lines) {
            break;
        }
        --start;
        ++bytes;
        if (ch == '\n') {
            ++lines;
        }
    }

    if (start > 0) {
        while (start < input.size() &&
               (static_cast<unsigned char>(input[start]) & 0xc0) == 0x80) {
            ++start;
        }
        const auto marker = input.rfind(kRedactionMarker, start);
        if (marker != std::string::npos && marker < start &&
            marker + kRedactionMarker.size() > start) {
            start = marker + kRedactionMarker.size();
        }
    }

    return OutputLimitResult{
        .text = bounded_utf8(std::string_view(input).substr(start), limit.max_bytes),
        .truncated = start > 0,
    };
}

[[nodiscard]] inline OutputLimitResult limit_output_tail_redacted(
    std::string input,
    OutputLimit limit = {}) {
    return limit_output_tail(redact_text(std::move(input)), limit);
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
        result.text = bounded_utf8(input, limit.max_bytes);
        result.truncated = input.size() > limit.max_bytes;
    } else if (!input.empty() && input.back() != '\n' && !result.truncated && !result.text.empty()) {
        result.text.pop_back();
    }
    if (result.truncated) {
        result.text += "\n[output truncated]";
    }
    return result;
}

} // namespace cch::util

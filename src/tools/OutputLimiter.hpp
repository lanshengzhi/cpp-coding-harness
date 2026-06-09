#pragma once

#include <sstream>
#include <string>
#include <vector>

namespace cch::tools {

struct OutputLimit {
    std::size_t max_bytes{50 * 1024};
    std::size_t max_lines{2000};
};

struct OutputLimitResult {
    std::string text;
    bool truncated{false};
};

inline OutputLimitResult limit_output(const std::string& input, OutputLimit limit = {}) {
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
    if (input.empty()) {
        result.text.clear();
    } else if (result.text.empty() && !input.empty()) {
        result.text = input.substr(0, limit.max_bytes);
        result.truncated = input.size() > limit.max_bytes;
    } else if (!input.empty() && input.back() != '\n' && !result.truncated && !result.text.empty()) {
        result.text.pop_back();
    }
    if (result.truncated) {
        result.text += "\n[output truncated]";
    }
    return result;
}

} // namespace cch::tools

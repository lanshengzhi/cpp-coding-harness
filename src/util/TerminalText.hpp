#pragma once

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>

namespace cch::util {

/// Remove CSI, OSC, and simple escape controls while preserving ordinary text.
[[nodiscard]] inline std::string strip_terminal_escape_sequences(
    std::string_view text) {
    std::string stripped;
    stripped.reserve(text.size());
    for (std::size_t index = 0; index < text.size();) {
        const auto value = static_cast<unsigned char>(text[index]);
        if (value != 0x1b) {
            stripped.push_back(text[index++]);
            continue;
        }
        if (index + 1 < text.size() && text[index + 1] == '[') {
            index += 2;
            while (index < text.size()) {
                const auto current = static_cast<unsigned char>(text[index++]);
                if (current >= 0x40 && current <= 0x7e) break;
            }
            continue;
        }
        if (index + 1 < text.size() && text[index + 1] == ']') {
            index += 2;
            while (index < text.size()) {
                if (text[index] == '\a') {
                    ++index;
                    break;
                }
                if (text[index] == '\x1b' && index + 1 < text.size() &&
                    text[index + 1] == '\\') {
                    index += 2;
                    break;
                }
                ++index;
            }
            continue;
        }
        index += std::min<std::size_t>(2, text.size() - index);
    }
    return stripped;
}

} // namespace cch::util

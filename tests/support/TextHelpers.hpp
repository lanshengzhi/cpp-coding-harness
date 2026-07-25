#pragma once

#include <cstddef>
#include <string_view>

namespace cch::tests {

/// Count the non-overlapping occurrences of `needle` inside `haystack`.
[[nodiscard]] inline std::size_t count_occurrences(
    std::string_view haystack,
    std::string_view needle) {
    if (needle.empty()) {
        return 0;
    }
    std::size_t count = 0;
    std::size_t position = 0;
    while ((position = haystack.find(needle, position)) != std::string_view::npos) {
        ++count;
        position += needle.size();
    }
    return count;
}

} // namespace cch::tests

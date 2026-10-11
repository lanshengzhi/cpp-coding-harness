#pragma once

#include <cch/support/Error.hpp>

#include <span>
#include <string_view>
#include <vector>

namespace cch::tui::detail {

/// ICU word boundaries match the frozen pi runtime; the API remains private to the TUI implementation.
struct WordSegment {
    std::size_t byte_start{0};
    std::size_t byte_end{0};
    bool word_like{false};
};

struct WordAtomicSpan {
    std::size_t byte_start{0};
    std::size_t byte_end{0};
};

[[nodiscard]] support::Expected<std::vector<WordSegment>> segment_words(std::string_view text);
[[nodiscard]] bool is_ecmascript_whitespace(std::string_view text);
[[nodiscard]] bool is_ascii_punctuation(char value);

/// Translate pi's cursor destination back to a UTF-8 byte offset.
[[nodiscard]] support::Expected<std::size_t> find_word_backward(
        std::string_view text, std::size_t cursor, std::span<const WordAtomicSpan> atomic_spans = {});
[[nodiscard]] support::Expected<std::size_t> find_word_forward(
        std::string_view text, std::size_t cursor, std::span<const WordAtomicSpan> atomic_spans = {});

} // namespace cch::tui::detail

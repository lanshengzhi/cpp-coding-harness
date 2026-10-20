#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace cch::chord {

/// Retention direction of an output window (pi `OutputLimits.retain`).
enum class OutputRetention {
    Head,
    Tail,
};

/// Retention limits of one output stream.
struct OutputLimits {
    std::size_t max_bytes{0};
    std::size_t max_lines{0};
    OutputRetention retain{OutputRetention::Head};
};

/// An exact slice of the input within the limits and what it left out (pi
/// `OutputSlice`).
struct OutputSlice {
    std::string text;
    std::size_t bytes{0};
    std::size_t dropped_bytes{0};
    std::size_t dropped_lines{0};
};

/// Retained output and what the limits dropped from the whole stream (pi
/// `BoundedOutput`).
struct BoundedOutput {
    std::string text;
    std::size_t dropped_bytes{0};
    std::size_t dropped_lines{0};
};

/// UTF-8 byte length of `text` (pi `utf8ByteLength`). The input is already
/// UTF-8 bytes, so the length is the byte count.
[[nodiscard]] std::size_t utf8_byte_length(std::string_view text);

/// The last character boundary at or before `index` (pi `characterEnd`).
/// `index` is clamped to the text size; the result is a byte offset that does
/// not split a multibyte sequence.
[[nodiscard]] std::size_t character_end(std::string_view text, std::size_t index);

/// Bound `text` to whole lines within the limits: the first lines for head,
/// the last lines for tail. A single line longer than `max_bytes` is cut at
/// the byte limit on a character boundary (pi `boundOutput`).
[[nodiscard]] OutputSlice bound_output(std::string_view text, OutputLimits limits);

/// Bounded running output of one stream (pi `OutputBuffer`). Accepting a chunk
/// costs time proportional to the chunk; counts of the whole stream are kept so
/// the dropped totals stay exact. String chunks are used because the C++ tool
/// boundary already decodes output to text.
class OutputBuffer {
public:
    explicit OutputBuffer(OutputLimits limits) : limits_(limits) {}

    /// Accept a chunk; returns whether anything was accepted.
    [[nodiscard]] bool push(std::string_view chunk);

    /// Retained, display-sanitized output and what the limits dropped from the
    /// whole stream. `dropped_bytes` and `dropped_lines` describe the raw
    /// retained window before sanitization.
    [[nodiscard]] BoundedOutput snapshot();

    [[nodiscard]] std::size_t stored_bytes() const noexcept { return stored_bytes_; }

private:
    struct StoredChunk {
        std::string text;
        std::size_t bytes{0};
        std::size_t newlines{0};
    };

    OutputLimits limits_;
    std::vector<StoredChunk> chunks_;
    std::size_t stored_bytes_{0};
    std::size_t stored_newlines_{0};
    bool full_{false};
    std::size_t total_bytes_{0};
    std::size_t total_newlines_{0};
    bool ends_with_newline_{true};
};

} // namespace cch::chord

#pragma once

#include <chrono>
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

/// Frozen pi `PROGRESS_BYTES_PER_SECOND` (`packages/chord/src/output.ts`):
/// every progress commit buys a pause proportional to what it wrote at this
/// byte rate (100 KiB/s).
inline constexpr std::size_t kProgressBytesPerSecond = 100 * 1024;

/// A caller-owned clock reading for progress pacing: milliseconds since an
/// arbitrary epoch. It is fractional because a byte-rate pause is not a whole
/// millisecond (pi `Progress.#nextAt` is a JavaScript number).
using ProgressTime = std::chrono::duration<double, std::milli>;

/// The pause a successful progress commit buys (pi `Progress.#flush`): at
/// least `min_interval`, otherwise the exact time `bytes` take at
/// `kProgressBytesPerSecond`. The fraction is kept, never rounded to a whole
/// millisecond.
[[nodiscard]] ProgressTime progress_pause(std::size_t bytes, std::chrono::milliseconds min_interval);

/// Timer-free pi `Progress` pacing. The caller owns the clock and the commit:
/// `mark` records a change, `due` says when the next coalesced commit may
/// start, and `committed` records what that commit wrote. No timer and no
/// clock is read here, so a declared schedule is reproducible.
class ProgressPacer {
public:
    explicit ProgressPacer(std::chrono::milliseconds min_interval) noexcept : min_interval_(min_interval) {}

    /// pi `mark`: include a change in the next commit.
    void mark() noexcept { pending_ = true; }

    /// Whether a marked change still waits for a commit.
    [[nodiscard]] bool pending() const noexcept { return pending_; }

    /// Earliest time the next marked change may commit (pi `#nextAt`).
    [[nodiscard]] ProgressTime next_commit_at() const noexcept { return next_commit_at_; }

    /// Whether the marked change may commit at `now` (pi `#schedule`'s
    /// `wait <= 0`). `now` and `next_commit_at()` share one time base.
    [[nodiscard]] bool due(ProgressTime now) const noexcept { return pending_ && now >= next_commit_at_; }

    /// Start the commit due now: the mark is consumed (pi `#flush` clears
    /// `#dirty`). A mark that arrives during the commit stays pending.
    void begin() noexcept { pending_ = false; }

    /// Complete the commit that started at `started` and wrote `bytes`, so the
    /// next marked change may commit at
    /// `started + progress_pause(bytes, min_interval)`.
    void committed(ProgressTime started, std::size_t bytes) noexcept {
        next_commit_at_ = started + progress_pause(bytes, min_interval_);
    }

private:
    std::chrono::milliseconds min_interval_;
    ProgressTime next_commit_at_{};
    bool pending_{false};
};

} // namespace cch::chord

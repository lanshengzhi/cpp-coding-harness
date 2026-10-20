#include <cch/chord/Output.hpp>

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

namespace cch::chord {

namespace {

constexpr char kNewline = '\n';

[[nodiscard]] std::size_t count_newlines(std::string_view text) {
    std::size_t newlines = 0;
    for (std::size_t index = text.find(kNewline); index != std::string_view::npos;
            index = text.find(kNewline, index + 1)) {
        ++newlines;
    }
    return newlines;
}

[[nodiscard]] std::size_t count_lines(std::string_view text) {
    if (text.empty()) return 0;
    return count_newlines(text) + (text.back() == kNewline ? 0 : 1);
}

// pi `sanitizeOutput`: remove control characters that break display and
// transcripts; tabs and newlines stay. The byte scan is exact for the UTF-8
// text this package receives: no continuation byte falls in the removed ASCII
// ranges, and U+FFF9..U+FFFB is removed as one three-byte sequence.
[[nodiscard]] std::string sanitize_output(std::string_view text) {
    std::string sanitized;
    sanitized.reserve(text.size());
    for (std::size_t index = 0; index < text.size();) {
        const auto byte = static_cast<unsigned char>(text[index]);
        if (byte <= 0x08 || (byte >= 0x0b && byte <= 0x1f)) {
            ++index;
            continue;
        }
        if (index + 2 < text.size() && byte == 0xef && static_cast<unsigned char>(text[index + 1]) == 0xbf &&
                static_cast<unsigned char>(text[index + 2]) >= 0xb9 &&
                static_cast<unsigned char>(text[index + 2]) <= 0xbb) {
            index += 3;
            continue;
        }
        sanitized.push_back(text[index]);
        ++index;
    }
    return sanitized;
}

[[nodiscard]] std::size_t character_start(std::string_view text, std::size_t index) {
    std::size_t start = index;
    while (start < text.size() && (static_cast<unsigned char>(text[start]) & 0xc0) == 0x80)
        ++start;
    return start;
}

[[nodiscard]] std::pair<std::size_t, std::size_t> head_range(std::string_view text, OutputLimits limits) {
    if (limits.max_lines == 0 || limits.max_bytes == 0) return {0, 0};
    std::size_t end = text.size();
    std::size_t lines = 0;
    for (std::size_t index = text.find(kNewline); index != std::string_view::npos;
            index = text.find(kNewline, index + 1)) {
        if (++lines == limits.max_lines) {
            end = index + 1;
            break;
        }
    }
    if (end > limits.max_bytes) {
        const std::size_t newline = text.rfind(kNewline, limits.max_bytes - 1);
        end = newline == std::string_view::npos ? character_end(text, limits.max_bytes) : newline + 1;
    }
    return {0, end};
}

[[nodiscard]] std::pair<std::size_t, std::size_t> tail_range(std::string_view text, OutputLimits limits) {
    if (limits.max_lines == 0 || limits.max_bytes == 0) return {text.size(), text.size()};
    // A trailing newline ends the last line rather than starting another.
    const std::ptrdiff_t last =
            static_cast<std::ptrdiff_t>(text.size()) - (text.empty() || text.back() != kNewline ? 1 : 2);
    std::size_t start = 0;
    std::size_t lines = 1;
    if (last >= 0) {
        std::size_t index = text.rfind(kNewline, static_cast<std::size_t>(last));
        while (index != std::string_view::npos) {
            if (lines == limits.max_lines) {
                start = index + 1;
                break;
            }
            ++lines;
            index = index == 0 ? std::string_view::npos : text.rfind(kNewline, index - 1);
        }
    }
    if (text.size() - start > limits.max_bytes) {
        const std::size_t from = text.size() - limits.max_bytes;
        const std::size_t newline = text.find(kNewline, from == 0 ? 0 : from - 1);
        // The first line starting inside the byte window, or a cut of the last
        // line when it alone is too long.
        start = newline != std::string_view::npos && newline + 1 < text.size() ? newline + 1
                                                                               : character_start(text, from);
    }
    return {start, text.size()};
}

[[nodiscard]] std::string tail_margin(std::string_view text, OutputLimits limits) {
    const std::size_t byte_start =
            text.size() > limits.max_bytes ? character_end(text, text.size() - limits.max_bytes - 1) : 0;
    std::size_t line_start = 0;
    std::size_t newlines = 0;
    std::size_t index = text.rfind(kNewline);
    while (index != std::string_view::npos) {
        if (++newlines > limits.max_lines) {
            line_start = index;
            break;
        }
        if (index == 0) break;
        index = text.rfind(kNewline, index - 1);
    }
    const std::size_t start = std::max(byte_start, line_start);
    return start == 0 ? std::string{text} : std::string{text.substr(start)};
}

} // namespace

std::size_t utf8_byte_length(std::string_view text) { return text.size(); }

std::size_t character_end(std::string_view text, std::size_t index) {
    if (index >= text.size()) return text.size();
    std::size_t end = index;
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xc0) == 0x80)
        --end;
    return end;
}

OutputSlice bound_output(std::string_view text, OutputLimits limits) {
    const auto [from, to] =
            limits.retain == OutputRetention::Head ? head_range(text, limits) : tail_range(text, limits);
    const auto kept = text.substr(from, to - from);
    return OutputSlice{
            .text = std::string{kept},
            .bytes = kept.size(),
            .dropped_bytes = text.size() - kept.size(),
            .dropped_lines = count_lines(text) - count_lines(kept),
    };
}

bool OutputBuffer::push(std::string_view chunk) {
    if (chunk.empty()) return false;

    const auto bytes = utf8_byte_length(chunk);
    const auto newlines = count_newlines(chunk);
    total_bytes_ += bytes;
    total_newlines_ += newlines;
    ends_with_newline_ = chunk.back() == kNewline;
    if (full_) return true;

    chunks_.push_back(StoredChunk{.text = std::string{chunk}, .bytes = bytes, .newlines = newlines});
    stored_bytes_ += bytes;
    stored_newlines_ += newlines;

    if (limits_.retain == OutputRetention::Head) {
        // Nothing past a full window is ever needed.
        full_ = stored_bytes_ > limits_.max_bytes || stored_newlines_ >= limits_.max_lines;
        return true;
    }

    // Drop leading chunks while the rest still holds more than a window: more
    // than max_bytes bytes or max_lines newlines, plus one, so the window's
    // line start can still be found. Each chunk is dropped once.
    while (chunks_.size() > 1) {
        const auto& first = chunks_.front();
        const auto bytes_after = stored_bytes_ - first.bytes;
        const auto newlines_after = stored_newlines_ - first.newlines;
        if (bytes_after <= limits_.max_bytes + 1 && newlines_after <= limits_.max_lines + 1) break;
        stored_bytes_ = bytes_after;
        stored_newlines_ = newlines_after;
        chunks_.erase(chunks_.begin());
    }
    return true;
}

BoundedOutput OutputBuffer::snapshot() {
    std::string stored;
    for (const auto& chunk : chunks_)
        stored += chunk.text;

    const auto kept = bound_output(stored, limits_);
    const auto stored_lines = count_lines(stored);
    const auto kept_lines = stored_lines - kept.dropped_lines;

    // Tail windows never reach back before this one, but finding a later
    // window's first line needs what precedes it: keep the shortest suffix
    // longer than the window by a byte or a line, as push does.
    if (limits_.retain == OutputRetention::Tail || chunks_.size() > 1) {
        const auto text = limits_.retain == OutputRetention::Tail ? tail_margin(stored, limits_) : stored;
        const auto bytes = limits_.retain == OutputRetention::Tail ? utf8_byte_length(text) : stored_bytes_;
        chunks_.clear();
        if (!text.empty()) {
            chunks_.push_back(StoredChunk{
                    .text = text,
                    .bytes = bytes,
                    .newlines = count_newlines(text),
            });
        }
        stored_bytes_ = text.empty() ? 0 : bytes;
        stored_newlines_ = chunks_.empty() ? 0 : chunks_.front().newlines;
    }

    return BoundedOutput{
            .text = sanitize_output(kept.text),
            .dropped_bytes = total_bytes_ - kept.bytes,
            .dropped_lines = total_newlines_ + (ends_with_newline_ ? 0 : 1) - kept_lines,
    };
}

} // namespace cch::chord

#include "tui/WordNavigation.hpp"

#include "tui/UnicodeWidth.hpp"

#include <unicode/ubrk.h>
#include <unicode/uloc.h>
#include <unicode/utext.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::tui::detail {
namespace {

using BreakIterator = std::unique_ptr<UBreakIterator, decltype(&ubrk_close)>;
using TextView = std::unique_ptr<UText, decltype(&utext_close)>;

struct NavigationSegment {
    std::size_t byte_start{0};
    std::size_t byte_end{0};
    bool word_like{false};
    bool atomic{false};
};

[[nodiscard]] support::Error segmentation_error(std::string message) {
    return support::make_error(support::ErrorCode::Unknown, std::move(message));
}

[[nodiscard]] std::vector<NavigationSegment> merge_atomic_spans(
        const std::vector<WordSegment>& words, std::span<const WordAtomicSpan> atomic_spans) {
    std::vector<NavigationSegment> result;
    result.reserve(words.size() + atomic_spans.size());
    std::size_t word_index = 0;
    std::size_t atomic_index = 0;
    std::size_t position = 0;
    const auto end = words.empty() ? std::size_t{0} : words.back().byte_end;
    while (position < end) {
        while (atomic_index < atomic_spans.size() && atomic_spans[atomic_index].byte_end <= position) {
            ++atomic_index;
        }
        if (atomic_index < atomic_spans.size() && atomic_spans[atomic_index].byte_start == position) {
            const auto atomic = atomic_spans[atomic_index++];
            result.push_back({.byte_start = atomic.byte_start, .byte_end = atomic.byte_end, .atomic = true});
            position = atomic.byte_end;
            while (word_index < words.size() && words[word_index].byte_end <= position)
                ++word_index;
            continue;
        }
        while (word_index < words.size() && words[word_index].byte_end <= position)
            ++word_index;
        if (word_index >= words.size()) break;
        const auto& word = words[word_index];
        const auto next_atomic = atomic_index < atomic_spans.size() ? atomic_spans[atomic_index].byte_start : end;
        const auto piece_end = std::min(word.byte_end, next_atomic);
        if (piece_end <= position) break;
        result.push_back({.byte_start = position, .byte_end = piece_end, .word_like = word.word_like});
        position = piece_end;
    }
    return result;
}

[[nodiscard]] std::vector<WordAtomicSpan> clip_atomic_spans(
        std::span<const WordAtomicSpan> atomic_spans, std::size_t begin, std::size_t end) {
    std::vector<WordAtomicSpan> result;
    for (const auto& span : atomic_spans) {
        if (span.byte_end <= begin || span.byte_start >= end) continue;
        result.push_back({.byte_start = std::max(span.byte_start, begin) - begin,
                .byte_end = std::min(span.byte_end, end) - begin});
    }
    return result;
}

[[nodiscard]] bool whitespace_segment(std::string_view text) {
    for (std::size_t offset = 0; offset < text.size();) {
        const auto [codepoint, bytes] = decode_utf8(text, offset);
        if (bytes == 0) return false;
        const bool ecmascript_whitespace =
                codepoint == 0x0009 || codepoint == 0x000A || codepoint == 0x000B || codepoint == 0x000C ||
                codepoint == 0x000D || codepoint == 0x0020 || codepoint == 0x00A0 || codepoint == 0x1680 ||
                (codepoint >= 0x2000 && codepoint <= 0x200A) || codepoint == 0x2028 || codepoint == 0x2029 ||
                codepoint == 0x202F || codepoint == 0x205F || codepoint == 0x3000 || codepoint == 0xFEFF;
        if (!ecmascript_whitespace) return false;
        offset += bytes;
    }
    return !text.empty();
}

} // namespace

support::Expected<std::vector<WordSegment>> segment_words(std::string_view text) {
    if (text.empty()) return std::vector<WordSegment>{};
    if (text.size() > static_cast<std::size_t>(std::numeric_limits<int64_t>::max())) {
        return std::unexpected(segmentation_error("ICU word segmentation input exceeds its supported length"));
    }

    UErrorCode status = U_ZERO_ERROR;
    BreakIterator iterator{ubrk_open(UBRK_WORD, uloc_getDefault(), nullptr, 0, &status), &ubrk_close};
    if (U_FAILURE(status) || !iterator) {
        return std::unexpected(segmentation_error("ICU could not create the editor word segmenter"));
    }

    status = U_ZERO_ERROR;
    TextView view{utext_openUTF8(nullptr, text.data(), static_cast<int64_t>(text.size()), &status), &utext_close};
    if (U_FAILURE(status) || !view) {
        return std::unexpected(segmentation_error("ICU could not open editor text as UTF-8"));
    }

    status = U_ZERO_ERROR;
    ubrk_setUText(iterator.get(), view.get(), &status);
    if (U_FAILURE(status)) {
        return std::unexpected(segmentation_error("ICU could not segment editor text"));
    }

    std::vector<WordSegment> result;
    auto start = ubrk_first(iterator.get());
    for (auto end = ubrk_next(iterator.get()); end != UBRK_DONE; start = end, end = ubrk_next(iterator.get())) {
        const auto rule_status = ubrk_getRuleStatus(iterator.get());
        result.push_back({.byte_start = static_cast<std::size_t>(start),
                .byte_end = static_cast<std::size_t>(end),
                .word_like = rule_status >= UBRK_WORD_NONE_LIMIT});
    }
    return result;
}

bool is_ecmascript_whitespace(std::string_view text) { return whitespace_segment(text); }

bool is_ascii_punctuation(char value) {
    constexpr std::string_view punctuation{"(){}[]<>.,;:'\"!?+-=*/\\|&%^$#@~`"};
    return punctuation.find(value) != std::string_view::npos;
}

support::Expected<std::size_t> find_word_backward(
        std::string_view text, std::size_t cursor, std::span<const WordAtomicSpan> atomic_spans) {
    cursor = std::min(cursor, text.size());
    if (cursor == 0) return 0;
    const auto prefix = text.substr(0, cursor);
    auto words = segment_words(prefix);
    if (!words) return std::unexpected(words.error());
    auto segments = merge_atomic_spans(*words, clip_atomic_spans(atomic_spans, 0, cursor));

    std::size_t skipped_whitespace_start = cursor;
    while (!segments.empty() && !segments.back().atomic &&
            whitespace_segment(
                    prefix.substr(segments.back().byte_start, segments.back().byte_end - segments.back().byte_start))) {
        skipped_whitespace_start = segments.back().byte_start;
        segments.pop_back();
    }
    if (segments.empty()) return skipped_whitespace_start;

    const auto& last = segments.back();
    if (last.atomic) return last.byte_start;
    if (last.word_like) {
        std::size_t last_punctuation = std::string_view::npos;
        for (std::size_t index = last.byte_start; index < last.byte_end; ++index) {
            if (is_ascii_punctuation(prefix[index])) last_punctuation = index;
        }
        return last_punctuation == std::string_view::npos ? last.byte_start : last_punctuation + 1;
    }

    auto punctuation_start = segments.back().byte_start;
    while (!segments.empty()) {
        const auto& segment = segments.back();
        if (segment.atomic || segment.word_like ||
                whitespace_segment(prefix.substr(segment.byte_start, segment.byte_end - segment.byte_start))) {
            break;
        }
        punctuation_start = segment.byte_start;
        segments.pop_back();
    }
    return segments.empty() ? punctuation_start : segments.back().byte_end;
}

support::Expected<std::size_t> find_word_forward(
        std::string_view text, std::size_t cursor, std::span<const WordAtomicSpan> atomic_spans) {
    cursor = std::min(cursor, text.size());
    if (cursor >= text.size()) return text.size();
    const auto suffix = text.substr(cursor);
    auto words = segment_words(suffix);
    if (!words) return std::unexpected(words.error());
    auto segments = merge_atomic_spans(*words, clip_atomic_spans(atomic_spans, cursor, text.size()));

    std::size_t skipped_whitespace_end = 0;
    std::size_t segment_index = 0;
    while (segment_index < segments.size() && !segments[segment_index].atomic &&
            whitespace_segment(suffix.substr(segments[segment_index].byte_start,
                    segments[segment_index].byte_end - segments[segment_index].byte_start))) {
        skipped_whitespace_end = segments[segment_index].byte_end;
        ++segment_index;
    }
    if (segment_index == segments.size()) return text.size();

    const auto& first = segments[segment_index];
    if (first.atomic) return cursor + first.byte_end;
    if (first.word_like) {
        for (std::size_t index = first.byte_start; index < first.byte_end; ++index) {
            if (is_ascii_punctuation(suffix[index])) return cursor + index;
        }
        return cursor + first.byte_end;
    }

    while (segment_index < segments.size()) {
        const auto& segment = segments[segment_index];
        if (segment.atomic || segment.word_like ||
                whitespace_segment(suffix.substr(segment.byte_start, segment.byte_end - segment.byte_start))) {
            break;
        }
        ++segment_index;
    }
    const auto punctuation_end = segment_index == 0 ? skipped_whitespace_end : segments[segment_index - 1].byte_end;
    return cursor + punctuation_end;
}

} // namespace cch::tui::detail

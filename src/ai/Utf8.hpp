#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace cch::ai {

/// Shared UTF-8 walking primitives. The wire payloads of every adapter are
/// UTF-8 text; these three functions are the one implementation of sequence
/// length, continuation validation, and code-point decoding.

/// Sequence length for a lead byte, or 0 for a continuation/invalid lead.
[[nodiscard]] inline std::size_t utf8_sequence_length(unsigned char lead) {
    if (lead <= 0x7f) {
        return 1;
    }
    if (lead >= 0xc2 && lead <= 0xdf) {
        return 2;
    }
    if (lead >= 0xe0 && lead <= 0xef) {
        return 3;
    }
    if (lead >= 0xf0 && lead <= 0xf4) {
        return 4;
    }
    return 0;
}

/// True when the `length`-byte sequence at `index` is in bounds, has valid
/// continuation bytes, and rejects overlong/surrogate/out-of-range forms.
[[nodiscard]] inline bool utf8_valid_sequence(std::string_view text, std::size_t index, std::size_t length) {
    if (index + length > text.size()) {
        return false;
    }
    for (std::size_t offset = 1; offset < length; ++offset) {
        const auto continuation = static_cast<unsigned char>(text[index + offset]);
        if (continuation < 0x80 || continuation > 0xbf) {
            return false;
        }
    }
    const auto lead = static_cast<unsigned char>(text[index]);
    if (length == 3) {
        const auto second = static_cast<unsigned char>(text[index + 1]);
        return !((lead == 0xe0 && second < 0xa0) || (lead == 0xed && second >= 0xa0));
    }
    if (length == 4) {
        const auto second = static_cast<unsigned char>(text[index + 1]);
        return !((lead == 0xf0 && second < 0x90) || (lead == 0xf4 && second >= 0x90));
    }
    return true;
}

/// Decodes the validated `length`-byte sequence at `index` to its code point.
[[nodiscard]] inline std::uint32_t utf8_decode_code_point(
        std::string_view value, std::size_t index, std::size_t length) {
    const auto lead = static_cast<unsigned char>(value[index]);
    if (length == 1) {
        return lead;
    }
    std::uint32_t result = lead & (length == 2 ? 0x1fU : length == 3 ? 0x0fU : 0x07U);
    for (std::size_t offset = 1; offset < length; ++offset) {
        result = (result << 6U) | (static_cast<unsigned char>(value[index + offset]) & 0x3fU);
    }
    return result;
}

/// Bytes occupied by the character at `index` under the tolerant estimate
/// walk: lead-range length with bounds/continuation checks (no
/// overlong/surrogate rejection); 1 whenever the sequence is not walkable so
/// callers always advance.
[[nodiscard]] inline std::size_t utf8_character_bytes(std::string_view text, std::size_t index) {
    const auto raw = utf8_sequence_length(static_cast<unsigned char>(text[index]));
    const auto length = raw == 0 ? 1 : raw;
    if (index + length > text.size()) {
        return 1;
    }
    for (std::size_t offset = 1; offset < length; ++offset) {
        const auto continuation = static_cast<unsigned char>(text[index + offset]);
        if (continuation < 0x80 || continuation > 0xbf) {
            return 1;
        }
    }
    return length;
}

} // namespace cch::ai

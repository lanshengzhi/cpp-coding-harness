#pragma once

#include "ai/Utf8.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace cch::ai::api {

/// Shared text and identifier primitives for the wire conversions:
/// UTF-8-sanitizing text passthrough and the pi-compatible id
/// normalization/hashing vocabulary.

inline constexpr std::string_view kUtf8Replacement = "\xef\xbf\xbd";

[[nodiscard]] inline std::string sanitize_text(std::string_view text) {
    std::string result;
    result.reserve(text.size());
    std::size_t index = 0;
    while (index < text.size()) {
        const auto length = utf8_sequence_length(static_cast<unsigned char>(text[index]));
        if (length == 0) {
            result += kUtf8Replacement;
            ++index;
            continue;
        }
        if (!utf8_valid_sequence(text, index, length)) {
            result += kUtf8Replacement;
            std::size_t consumed = 1;
            while (consumed < length && index + consumed < text.size()) {
                const auto continuation = static_cast<unsigned char>(text[index + consumed]);
                if (continuation < 0x80 || continuation > 0xbf) {
                    break;
                }
                ++consumed;
            }
            index += consumed;
            continue;
        }
        result.append(text, index, length);
        index += length;
    }
    return result;
}

[[nodiscard]] inline bool blank(std::string_view text) {
    for (std::size_t index = 0; index < text.size();) {
        const auto length = utf8_sequence_length(static_cast<unsigned char>(text[index]));
        if (length == 0 || !utf8_valid_sequence(text, index, length)) {
            return false;
        }
        const auto code_point = utf8_decode_code_point(text, index, length);
        const bool whitespace =
                code_point == 0x0009U || code_point == 0x000aU || code_point == 0x000bU || code_point == 0x000cU ||
                code_point == 0x000dU || code_point == 0x0020U || code_point == 0x00a0U || code_point == 0x1680U ||
                (code_point >= 0x2000U && code_point <= 0x200aU) || code_point == 0x2028U || code_point == 0x2029U ||
                code_point == 0x202fU || code_point == 0x205fU || code_point == 0x3000U || code_point == 0xfeffU;
        if (!whitespace) {
            return false;
        }
        index += length;
    }
    return true;
}

[[nodiscard]] inline std::string normalize_id_part(std::string_view value, bool trim_trailing) {
    std::string result;
    result.reserve(std::min<std::size_t>(64, value.size()));
    for (std::size_t index = 0; index < value.size() && result.size() < 64;) {
        const auto character = static_cast<unsigned char>(value[index]);
        const bool allowed = (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
                             (character >= '0' && character <= '9') || character == '_' || character == '-';
        if (allowed) {
            result.push_back(static_cast<char>(character));
            ++index;
            continue;
        }
        const auto length = utf8_sequence_length(character);
        const bool valid = length != 0 && utf8_valid_sequence(value, index, length);
        const auto replacement_count = valid && utf8_decode_code_point(value, index, length) > 0xffffU ? 2U : 1U;
        result.append(std::min<std::size_t>(replacement_count, 64 - result.size()), '_');
        index += valid ? length : 1;
    }
    if (trim_trailing) {
        while (!result.empty() && result.back() == '_') {
            result.pop_back();
        }
    }
    return result;
}

[[nodiscard]] inline std::string base36(std::uint32_t value) {
    constexpr char kDigits[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    std::string result;
    do {
        result.push_back(kDigits[value % 36]);
        value /= 36;
    } while (value != 0);
    std::ranges::reverse(result);
    return result;
}

[[nodiscard]] inline std::uint32_t multiply_wrapped(std::uint32_t left, std::uint32_t right) {
    return static_cast<std::uint32_t>(static_cast<std::uint64_t>(left) * static_cast<std::uint64_t>(right));
}

[[nodiscard]] inline std::string short_hash(std::string_view value) {
    std::uint32_t first = 0xdeadbeefU;
    std::uint32_t second = 0x41c6ce57U;
    const auto mix = [&first, &second](std::uint32_t code_unit) {
        first = multiply_wrapped(first ^ code_unit, 2654435761U);
        second = multiply_wrapped(second ^ code_unit, 1597334677U);
    };
    for (std::size_t index = 0; index < value.size();) {
        const auto length = utf8_sequence_length(static_cast<unsigned char>(value[index]));
        if (length == 0 || !utf8_valid_sequence(value, index, length)) {
            mix(0xfffdU);
            ++index;
            continue;
        }
        const auto code_point = utf8_decode_code_point(value, index, length);
        if (code_point <= 0xffffU) {
            mix(code_point);
        } else {
            const auto supplementary = code_point - 0x10000U;
            mix(0xd800U + (supplementary >> 10U));
            mix(0xdc00U + (supplementary & 0x3ffU));
        }
        index += length;
    }
    first = multiply_wrapped(first ^ (first >> 16), 2246822507U) ^
            multiply_wrapped(second ^ (second >> 13), 3266489909U);
    second = multiply_wrapped(second ^ (second >> 16), 2246822507U) ^
             multiply_wrapped(first ^ (first >> 13), 3266489909U);
    return base36(second) + base36(first);
}

[[nodiscard]] inline std::size_t utf16_length(std::string_view value) {
    std::size_t result = 0;
    for (std::size_t index = 0; index < value.size();) {
        const auto length = utf8_sequence_length(static_cast<unsigned char>(value[index]));
        if (length == 0 || !utf8_valid_sequence(value, index, length)) {
            ++result;
            ++index;
            continue;
        }
        result += utf8_decode_code_point(value, index, length) > 0xffffU ? 2 : 1;
        index += length;
    }
    return result;
}

[[nodiscard]] inline std::string bounded_message_id(std::string id) {
    return utf16_length(id) <= 64 ? id : "msg_" + short_hash(id);
}

} // namespace cch::ai::api

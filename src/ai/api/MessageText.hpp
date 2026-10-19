#pragma once

#include "ai/Utf8.hpp"

#include <string>
#include <string_view>

namespace cch::ai::api {

/// Shared text primitives for the wire conversions: UTF-8-sanitizing text
/// passthrough and the shared notion of blank text.

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

} // namespace cch::ai::api

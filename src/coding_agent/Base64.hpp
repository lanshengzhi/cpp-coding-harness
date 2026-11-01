#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace cch::coding_agent {

/// The standard base64 alphabet. Shared by the inline-image encoder and the
/// codemode output writer, which saves an `image()` block's bytes.
inline constexpr std::string_view kBase64Alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/// Decode base64 (or base64url) text. Node's `Buffer.from(value, "base64")`
/// ignores non-alphabet input and accepts unpadded data; the decoder mirrors
/// that permissive view so a payload the baseline accepted is not rejected
/// here.
[[nodiscard]] inline std::vector<std::uint8_t> decode_base64(std::string_view encoded) {
    std::array<int, 256> digits{};
    digits.fill(-1);
    for (std::size_t index = 0; index < kBase64Alphabet.size(); ++index) {
        digits[static_cast<unsigned char>(kBase64Alphabet[index])] = static_cast<int>(index);
    }
    digits[static_cast<unsigned char>('-')] = 62;
    digits[static_cast<unsigned char>('_')] = 63;

    std::vector<std::uint8_t> decoded;
    decoded.reserve(encoded.size() * 3U / 4U);
    std::uint32_t bits = 0;
    std::size_t bit_count = 0;
    for (const auto character : encoded) {
        if (character == '=') break;
        const auto digit = digits[static_cast<unsigned char>(character)];
        if (digit < 0) continue;
        bits = (bits << 6U) | static_cast<std::uint32_t>(digit);
        bit_count += 6U;
        if (bit_count >= 8U) {
            bit_count -= 8U;
            decoded.push_back(static_cast<std::uint8_t>((bits >> bit_count) & 0xffU));
        }
    }
    return decoded;
}

} // namespace cch::coding_agent

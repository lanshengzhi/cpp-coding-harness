#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace cch::tui::detail {

/// Standard base64 with padding (pi `Buffer.toString("base64")`). One helper
/// for the owning package: the program status message, the iTerm2 image
/// filename parameter, and the clipboard selection payload all encode the same
/// way.
[[nodiscard]] inline std::string base64_encode(std::string_view value) {
    constexpr std::string_view kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    result.reserve((value.size() + 2) / 3 * 4);
    for (std::size_t offset = 0; offset < value.size(); offset += 3) {
        const auto remaining = value.size() - offset;
        const auto first = static_cast<std::uint32_t>(static_cast<unsigned char>(value[offset]));
        const auto second =
                remaining > 1 ? static_cast<std::uint32_t>(static_cast<unsigned char>(value[offset + 1])) : 0U;
        const auto third =
                remaining > 2 ? static_cast<std::uint32_t>(static_cast<unsigned char>(value[offset + 2])) : 0U;
        const auto bits = (first << 16U) | (second << 8U) | third;
        result.push_back(kAlphabet[(bits >> 18U) & 0x3fU]);
        result.push_back(kAlphabet[(bits >> 12U) & 0x3fU]);
        result.push_back(remaining > 1 ? kAlphabet[(bits >> 6U) & 0x3fU] : '=');
        result.push_back(remaining > 2 ? kAlphabet[bits & 0x3fU] : '=');
    }
    return result;
}

} // namespace cch::tui::detail

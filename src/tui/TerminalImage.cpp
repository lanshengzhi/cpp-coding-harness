#include "TerminalImage.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>

namespace cch::tui::detail {
namespace {

constexpr std::size_t kKittyChunkSize = 4096;

[[nodiscard]] bool intervals_intersect(
    std::size_t left_start,
    std::size_t left_size,
    std::size_t right_start,
    std::size_t right_size) {
    if (left_size == 0 || right_size == 0) return false;
    if (left_start <= right_start) return right_start - left_start < left_size;
    return left_start - right_start < right_size;
}

[[nodiscard]] std::string base64_encode(std::string_view value) {
    constexpr std::string_view kAlphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    for (std::size_t offset = 0; offset < value.size(); offset += 3) {
        const auto remaining = value.size() - offset;
        const auto first = static_cast<std::uint32_t>(static_cast<unsigned char>(value[offset]));
        const auto second = remaining > 1
            ? static_cast<std::uint32_t>(static_cast<unsigned char>(value[offset + 1]))
            : 0U;
        const auto third = remaining > 2
            ? static_cast<std::uint32_t>(static_cast<unsigned char>(value[offset + 2]))
            : 0U;
        const auto bits = (first << 16U) | (second << 8U) | third;
        result.push_back(kAlphabet[(bits >> 18U) & 0x3fU]);
        result.push_back(kAlphabet[(bits >> 12U) & 0x3fU]);
        result.push_back(remaining > 1 ? kAlphabet[(bits >> 6U) & 0x3fU] : '=');
        result.push_back(remaining > 2 ? kAlphabet[bits & 0x3fU] : '=');
    }
    return result;
}

[[nodiscard]] std::string encode_kitty(
    const TerminalImage& image,
    TerminalImageHandle handle) {
    auto parameters = std::format(
        "a=T,f=100,q=2,C=1,c={},r={},i={}",
        image.region.columns,
        image.region.rows,
        handle.value);
    if (image.encoded_data.size() <= kKittyChunkSize) {
        return std::format("\x1b_G{};{}\x1b\\", parameters, image.encoded_data);
    }

    std::string result;
    for (std::size_t offset = 0; offset < image.encoded_data.size(); offset += kKittyChunkSize) {
        const auto size = std::min(kKittyChunkSize, image.encoded_data.size() - offset);
        const auto last = offset + size == image.encoded_data.size();
        if (offset == 0) {
            result += std::format(
                "\x1b_G{},m=1;{}\x1b\\",
                parameters,
                image.encoded_data.substr(offset, size));
        } else {
            result += std::format(
                "\x1b_Gm={};{}\x1b\\",
                last ? 0 : 1,
                image.encoded_data.substr(offset, size));
        }
    }
    return result;
}

[[nodiscard]] std::string encode_iterm2(const TerminalImage& image) {
    auto parameters = std::format(
        "inline=1;width={};height={};preserveAspectRatio=1",
        image.region.columns,
        image.region.rows);
    if (image.filename) parameters += ";name=" + base64_encode(*image.filename);
    return std::format("\x1b]1337;File={}:{}\x07", parameters, image.encoded_data);
}

} // namespace

bool cell_regions_intersect(
    const CellRegion& left,
    const CellRegion& right) {
    return intervals_intersect(left.column, left.columns, right.column, right.columns) &&
        intervals_intersect(left.row, left.rows, right.row, right.rows);
}

bool protocol_supports_mime(
    InlineImageProtocol protocol,
    std::string_view mime_type) {
    if (protocol == InlineImageProtocol::Kitty) return mime_type == "image/png";
    if (protocol == InlineImageProtocol::ITerm2) {
        return mime_type == "image/png" || mime_type == "image/jpeg" ||
            mime_type == "image/gif" || mime_type == "image/webp";
    }
    return false;
}

util::Expected<std::string> encode_terminal_image(
    InlineImageProtocol protocol,
    const TerminalImage& image,
    TerminalImageHandle handle) {
    if (image.region.columns == 0 || image.region.rows == 0) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "Inline image region must be positive"));
    }
    if (!protocol_supports_mime(protocol, image.mime_type)) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "Inline image format is unsupported by the terminal protocol"));
    }
    if (protocol == InlineImageProtocol::Kitty) return encode_kitty(image, handle);
    if (protocol == InlineImageProtocol::ITerm2) return encode_iterm2(image);
    return std::unexpected(util::make_error(
        util::ErrorCode::Validation,
        "Terminal does not support inline images"));
}

util::Expected<std::string> encode_terminal_image_removal(
    InlineImageProtocol protocol,
    TerminalImageHandle handle) {
    if (protocol == InlineImageProtocol::Kitty) {
        return std::format("\x1b_Ga=d,d=I,i={},q=2\x1b\\", handle.value);
    }
    if (protocol == InlineImageProtocol::ITerm2) return std::string{};
    return std::unexpected(util::make_error(
        util::ErrorCode::Validation,
        "Terminal does not support inline images"));
}

} // namespace cch::tui::detail

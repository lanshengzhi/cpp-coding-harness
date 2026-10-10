#include <cch/tui/TerminalImage.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace cch::tui {
namespace {

constexpr std::size_t kMaxDecodedBytes = 64U * 1024U * 1024U;

[[nodiscard]] int base64_digit(char value) {
    if (value >= 'A' && value <= 'Z') return value - 'A';
    if (value >= 'a' && value <= 'z') return value - 'a' + 26;
    if (value >= '0' && value <= '9') return value - '0' + 52;
    if (value == '+' || value == '-') return 62;
    if (value == '/' || value == '_') return 63;
    return -1;
}

/// Node Buffer.from(..., "base64") ignores non-alphabet input and accepts
/// unpadded/base64url data; dimensions use that same permissive decoded view.
[[nodiscard]] std::optional<std::vector<std::uint8_t>> decode_base64(std::string_view encoded) {
    std::vector<std::uint8_t> decoded;
    decoded.reserve(encoded.size() * 3U / 4U);
    std::uint32_t bits = 0;
    unsigned bit_count = 0;
    for (const auto character : encoded) {
        if (character == '=') break;
        const auto digit = base64_digit(character);
        if (digit < 0) continue;
        bits = (bits << 6U) | static_cast<std::uint32_t>(digit);
        bit_count += 6U;
        if (bit_count >= 8U) {
            bit_count -= 8U;
            decoded.push_back(static_cast<std::uint8_t>((bits >> bit_count) & 0xffU));
            if (decoded.size() > kMaxDecodedBytes) return std::nullopt;
        }
    }
    return decoded;
}

[[nodiscard]] std::uint16_t read_be16(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    return static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(bytes[offset]) << 8U) | static_cast<std::uint16_t>(bytes[offset + 1]));
}

[[nodiscard]] std::uint32_t read_be32(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    return (static_cast<std::uint32_t>(bytes[offset]) << 24U) | (static_cast<std::uint32_t>(bytes[offset + 1]) << 16U) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 8U) | static_cast<std::uint32_t>(bytes[offset + 3]);
}

[[nodiscard]] std::uint16_t read_le16(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    return static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(bytes[offset]) | (static_cast<std::uint16_t>(bytes[offset + 1]) << 8U));
}

[[nodiscard]] std::uint32_t read_le32(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    return static_cast<std::uint32_t>(bytes[offset]) | (static_cast<std::uint32_t>(bytes[offset + 1]) << 8U) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16U) |
           (static_cast<std::uint32_t>(bytes[offset + 3]) << 24U);
}

[[nodiscard]] bool bytes_equal(const std::vector<std::uint8_t>& bytes, std::size_t offset, std::string_view expected) {
    if (offset > bytes.size() || expected.size() > bytes.size() - offset) return false;
    for (std::size_t index = 0; index < expected.size(); ++index) {
        if (bytes[offset + index] != static_cast<std::uint8_t>(expected[index])) return false;
    }
    return true;
}

[[nodiscard]] std::optional<ImagePixelSize> png_dimensions(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() < 24 || !bytes_equal(bytes, 0, "\x89PNG")) return std::nullopt;
    return ImagePixelSize{.width = read_be32(bytes, 16), .height = read_be32(bytes, 20)};
}

[[nodiscard]] std::optional<ImagePixelSize> jpeg_dimensions(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() < 2 || bytes[0] != 0xff || bytes[1] != 0xd8) return std::nullopt;

    std::size_t offset = 2;
    while (offset < bytes.size() && bytes.size() - offset > 9) {
        if (bytes[offset] != 0xff) {
            ++offset;
            continue;
        }
        const auto marker = bytes[offset + 1];
        if (marker >= 0xc0 && marker <= 0xc2) {
            if (bytes.size() - offset < 9) return std::nullopt;
            return ImagePixelSize{
                    .width = read_be16(bytes, offset + 7),
                    .height = read_be16(bytes, offset + 5),
            };
        }
        if (bytes.size() - offset < 4) return std::nullopt;
        const auto length = static_cast<std::size_t>(read_be16(bytes, offset + 2));
        if (length < 2) return std::nullopt;
        if (length > bytes.size() - offset - 2) return std::nullopt;
        offset += 2 + length;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<ImagePixelSize> gif_dimensions(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() < 10 || (!bytes_equal(bytes, 0, "GIF87a") && !bytes_equal(bytes, 0, "GIF89a"))) {
        return std::nullopt;
    }
    return ImagePixelSize{.width = read_le16(bytes, 6), .height = read_le16(bytes, 8)};
}

[[nodiscard]] std::optional<ImagePixelSize> webp_dimensions(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() < 30 || !bytes_equal(bytes, 0, "RIFF") || !bytes_equal(bytes, 8, "WEBP")) {
        return std::nullopt;
    }

    if (bytes_equal(bytes, 12, "VP8 ")) {
        const auto width = read_le16(bytes, 26) & 0x3fffU;
        const auto height = read_le16(bytes, 28) & 0x3fffU;
        return ImagePixelSize{.width = width, .height = height};
    }
    if (bytes_equal(bytes, 12, "VP8L")) {
        if (bytes.size() < 25) return std::nullopt;
        const auto bits = read_le32(bytes, 21);
        return ImagePixelSize{
                .width = (bits & 0x3fffU) + 1U,
                .height = ((bits >> 14U) & 0x3fffU) + 1U,
        };
    }
    if (bytes_equal(bytes, 12, "VP8X")) {
        const auto width = static_cast<std::uint32_t>(bytes[24]) | (static_cast<std::uint32_t>(bytes[25]) << 8U) |
                           (static_cast<std::uint32_t>(bytes[26]) << 16U);
        const auto height = static_cast<std::uint32_t>(bytes[27]) | (static_cast<std::uint32_t>(bytes[28]) << 8U) |
                            (static_cast<std::uint32_t>(bytes[29]) << 16U);
        return ImagePixelSize{.width = width + 1U, .height = height + 1U};
    }
    return std::nullopt;
}

} // namespace

std::optional<ImagePixelSize> get_png_dimensions(std::string_view base64_data) {
    const auto decoded = decode_base64(base64_data);
    return decoded ? png_dimensions(*decoded) : std::nullopt;
}

std::optional<ImagePixelSize> get_jpeg_dimensions(std::string_view base64_data) {
    const auto decoded = decode_base64(base64_data);
    return decoded ? jpeg_dimensions(*decoded) : std::nullopt;
}

std::optional<ImagePixelSize> get_gif_dimensions(std::string_view base64_data) {
    const auto decoded = decode_base64(base64_data);
    return decoded ? gif_dimensions(*decoded) : std::nullopt;
}

std::optional<ImagePixelSize> get_webp_dimensions(std::string_view base64_data) {
    const auto decoded = decode_base64(base64_data);
    return decoded ? webp_dimensions(*decoded) : std::nullopt;
}

std::optional<ImagePixelSize> get_image_dimensions(std::string_view base64_data, std::string_view mime_type) {
    if (mime_type == "image/png") return get_png_dimensions(base64_data);
    if (mime_type == "image/jpeg") return get_jpeg_dimensions(base64_data);
    if (mime_type == "image/gif") return get_gif_dimensions(base64_data);
    if (mime_type == "image/webp") return get_webp_dimensions(base64_data);
    return std::nullopt;
}

} // namespace cch::tui

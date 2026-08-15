#include "ImageInput.hpp"

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include <stb_image_resize2.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#include <webp/decode.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <format>
#include <limits>
#include <string>
#include <utility>

namespace cch::coding_agent {
namespace {

constexpr std::string_view kBase64Alphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
constexpr std::string_view kResizeOmission =
    "[Image omitted: could not be resized below the inline image size limit.]";

struct ImageDimensions {
    std::size_t width;
    std::size_t height;
};

struct DecodedImage {
    std::vector<std::uint8_t> rgba;
    ImageDimensions dimensions;
};

[[nodiscard]] bool has_prefix(
    std::span<const std::uint8_t> bytes,
    std::span<const std::uint8_t> prefix) {
    return bytes.size() >= prefix.size() &&
        std::equal(prefix.begin(), prefix.end(), bytes.begin());
}

[[nodiscard]] bool bytes_equal(
    std::span<const std::uint8_t> bytes,
    std::size_t offset,
    std::string_view expected) {
    if (offset > bytes.size() || expected.size() > bytes.size() - offset) return false;
    for (std::size_t index = 0; index < expected.size(); ++index) {
        if (bytes[offset + index] != static_cast<std::uint8_t>(expected[index])) return false;
    }
    return true;
}

[[nodiscard]] std::uint32_t big_endian_u32(const std::uint8_t* bytes) {
    return (static_cast<std::uint32_t>(bytes[0]) << 24U) |
        (static_cast<std::uint32_t>(bytes[1]) << 16U) |
        (static_cast<std::uint32_t>(bytes[2]) << 8U) |
        static_cast<std::uint32_t>(bytes[3]);
}

[[nodiscard]] bool is_static_png(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < 16 || big_endian_u32(bytes.data() + 8) != 13 ||
        !bytes_equal(bytes, 12, "IHDR")) {
        return false;
    }
    std::size_t offset = 8;
    while (offset + 8 <= bytes.size()) {
        const auto chunk_length = static_cast<std::size_t>(big_endian_u32(bytes.data() + offset));
        const auto chunk_type_offset = offset + 4;
        if (bytes_equal(bytes, chunk_type_offset, "acTL")) return false;
        if (bytes_equal(bytes, chunk_type_offset, "IDAT")) return true;
        if (chunk_length > bytes.size() - offset - 8 ||
            chunk_length + 12 > bytes.size() - offset) {
            return true;
        }
        offset += chunk_length + 12;
    }
    return true;
}

[[nodiscard]] std::uint16_t read_u16(
    std::span<const std::uint8_t> bytes,
    std::size_t offset,
    bool little_endian) {
    if (little_endian) {
        return static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(bytes[offset]) |
            (static_cast<std::uint16_t>(bytes[offset + 1]) << 8U));
    }
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(bytes[offset]) << 8U) |
        static_cast<std::uint16_t>(bytes[offset + 1]));
}

[[nodiscard]] std::uint32_t read_u32(
    std::span<const std::uint8_t> bytes,
    std::size_t offset,
    bool little_endian) {
    if (little_endian) {
        return static_cast<std::uint32_t>(bytes[offset]) |
            (static_cast<std::uint32_t>(bytes[offset + 1]) << 8U) |
            (static_cast<std::uint32_t>(bytes[offset + 2]) << 16U) |
            (static_cast<std::uint32_t>(bytes[offset + 3]) << 24U);
    }
    return big_endian_u32(bytes.data() + offset);
}

[[nodiscard]] std::optional<int> tiff_orientation(
    std::span<const std::uint8_t> bytes) {
    if (bytes.size() < 8) return std::nullopt;
    const bool little_endian = bytes_equal(bytes, 0, "II");
    if (!little_endian && !bytes_equal(bytes, 0, "MM")) return std::nullopt;
    if (read_u16(bytes, 2, little_endian) != 42) return std::nullopt;

    const auto ifd_offset = static_cast<std::size_t>(read_u32(bytes, 4, little_endian));
    if (ifd_offset > bytes.size() || bytes.size() - ifd_offset < 2) return std::nullopt;
    const auto count = static_cast<std::size_t>(read_u16(bytes, ifd_offset, little_endian));
    const auto entries_offset = ifd_offset + 2;
    if (count > (bytes.size() - entries_offset) / 12) return std::nullopt;
    for (std::size_t index = 0; index < count; ++index) {
        const auto entry = entries_offset + index * 12;
        if (read_u16(bytes, entry, little_endian) != 0x0112 ||
            read_u16(bytes, entry + 2, little_endian) != 3 ||
            read_u32(bytes, entry + 4, little_endian) != 1) {
            continue;
        }
        const auto orientation = static_cast<int>(read_u16(bytes, entry + 8, little_endian));
        if (orientation >= 1 && orientation <= 8) return orientation;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<int> jpeg_orientation(
    std::span<const std::uint8_t> bytes) {
    if (bytes.size() < 4 || bytes[0] != 0xff || bytes[1] != 0xd8) return std::nullopt;
    std::size_t offset = 2;
    while (offset + 4 <= bytes.size()) {
        if (bytes[offset] != 0xff) {
            ++offset;
            continue;
        }
        while (offset < bytes.size() && bytes[offset] == 0xff) ++offset;
        if (offset >= bytes.size()) break;
        const auto marker = bytes[offset++];
        if (marker == 0xd9 || marker == 0xda) break;
        if (marker == 0x01 || (marker >= 0xd0 && marker <= 0xd7)) continue;
        if (offset + 2 > bytes.size()) break;
        const auto length = static_cast<std::size_t>(
            (static_cast<std::uint16_t>(bytes[offset]) << 8U) | bytes[offset + 1]);
        if (length < 2 || length > bytes.size() - offset) break;
        if (marker == 0xe1 && length >= 8 && bytes_equal(bytes, offset + 2, std::string_view{"Exif\0\0", 6})) {
            return tiff_orientation(bytes.subspan(offset + 8, length - 8));
        }
        offset += length;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<int> webp_orientation(
    std::span<const std::uint8_t> bytes) {
    if (!bytes_equal(bytes, 0, "RIFF") || !bytes_equal(bytes, 8, "WEBP")) {
        return std::nullopt;
    }
    std::size_t offset = 12;
    while (offset + 8 <= bytes.size()) {
        const auto length = static_cast<std::size_t>(read_u32(bytes, offset + 4, true));
        const auto payload = offset + 8;
        if (length > bytes.size() - payload) return std::nullopt;
        if (bytes_equal(bytes, offset, "EXIF")) {
            const auto exif = bytes.subspan(payload, length);
            return bytes_equal(exif, 0, std::string_view{"Exif\0\0", 6})
                ? tiff_orientation(exif.subspan(6))
                : tiff_orientation(exif);
        }
        const auto padded_length = length + (length & 1U);
        if (padded_length > bytes.size() - payload) break;
        offset = payload + padded_length;
    }
    return std::nullopt;
}

[[nodiscard]] int image_orientation(
    std::span<const std::uint8_t> bytes,
    std::string_view mime_type) {
    const auto orientation = mime_type == "image/jpeg"
        ? jpeg_orientation(bytes)
        : mime_type == "image/webp" ? webp_orientation(bytes) : std::nullopt;
    return orientation.value_or(1);
}

void apply_orientation(DecodedImage& image, int orientation) {
    if (orientation <= 1 || orientation > 8) return;
    const auto source_width = image.dimensions.width;
    const auto source_height = image.dimensions.height;
    const bool swaps_axes = orientation >= 5;
    const auto output_width = swaps_axes ? source_height : source_width;
    const auto output_height = swaps_axes ? source_width : source_height;
    std::vector<std::uint8_t> oriented(image.rgba.size());

    for (std::size_t y = 0; y < source_height; ++y) {
        for (std::size_t x = 0; x < source_width; ++x) {
            std::size_t output_x = x;
            std::size_t output_y = y;
            switch (orientation) {
            case 2:
                output_x = source_width - 1 - x;
                break;
            case 3:
                output_x = source_width - 1 - x;
                output_y = source_height - 1 - y;
                break;
            case 4:
                output_y = source_height - 1 - y;
                break;
            case 5:
                output_x = y;
                output_y = x;
                break;
            case 6:
                output_x = source_height - 1 - y;
                output_y = x;
                break;
            case 7:
                output_x = source_height - 1 - y;
                output_y = source_width - 1 - x;
                break;
            case 8:
                output_x = y;
                output_y = source_width - 1 - x;
                break;
            default:
                break;
            }
            const auto source = (y * source_width + x) * 4;
            const auto destination = (output_y * output_width + output_x) * 4;
            std::copy_n(image.rgba.data() + source, 4, oriented.data() + destination);
        }
    }
    image.rgba = std::move(oriented);
    image.dimensions = {.width = output_width, .height = output_height};
}

[[nodiscard]] std::string encode_base64(std::span<const std::uint8_t> bytes) {
    std::string encoded;
    encoded.reserve(((bytes.size() + 2) / 3) * 4);
    for (std::size_t index = 0; index < bytes.size(); index += 3) {
        const auto remaining = bytes.size() - index;
        const auto value = (static_cast<std::uint32_t>(bytes[index]) << 16U) |
            (remaining > 1 ? static_cast<std::uint32_t>(bytes[index + 1]) << 8U : 0U) |
            (remaining > 2 ? static_cast<std::uint32_t>(bytes[index + 2]) : 0U);
        encoded.push_back(kBase64Alphabet[(value >> 18U) & 0x3fU]);
        encoded.push_back(kBase64Alphabet[(value >> 12U) & 0x3fU]);
        encoded.push_back(remaining > 1 ? kBase64Alphabet[(value >> 6U) & 0x3fU] : '=');
        encoded.push_back(remaining > 2 ? kBase64Alphabet[value & 0x3fU] : '=');
    }
    return encoded;
}

[[nodiscard]] std::optional<DecodedImage> decode_image(
    std::span<const std::uint8_t> bytes,
    std::string_view mime_type) {
    int width = 0;
    int height = 0;
    std::uint8_t* decoded = nullptr;
    bool webp_allocation = false;
    if (mime_type == "image/webp") {
        decoded = WebPDecodeRGBA(bytes.data(), bytes.size(), &width, &height);
        webp_allocation = true;
    } else {
        if (bytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
            return std::nullopt;
        }
        int channels = 0;
        decoded = stbi_load_from_memory(
            bytes.data(),
            static_cast<int>(bytes.size()),
            &width,
            &height,
            &channels,
            STBI_rgb_alpha);
    }
    if (decoded == nullptr || width <= 0 || height <= 0) {
        if (decoded != nullptr) {
            if (webp_allocation) WebPFree(decoded);
            else stbi_image_free(decoded);
        }
        return std::nullopt;
    }

    const auto size = static_cast<std::size_t>(width) *
        static_cast<std::size_t>(height) * 4;
    DecodedImage image{
        .rgba = std::vector<std::uint8_t>(decoded, decoded + size),
        .dimensions = {
            .width = static_cast<std::size_t>(width),
            .height = static_cast<std::size_t>(height),
        },
    };
    if (webp_allocation) WebPFree(decoded);
    else stbi_image_free(decoded);
    apply_orientation(image, image_orientation(bytes, mime_type));
    return image;
}

void append_encoded_bytes(void* context, void* data, int size) {
    if (context == nullptr || data == nullptr || size <= 0) return;
    auto& bytes = *static_cast<std::vector<std::uint8_t>*>(context);
    const auto* first = static_cast<const std::uint8_t*>(data);
    bytes.insert(bytes.end(), first, first + size);
}

[[nodiscard]] std::vector<std::uint8_t> encode_png(
    std::span<const std::uint8_t> rgba,
    std::size_t width,
    std::size_t height) {
    std::vector<std::uint8_t> encoded;
    if (width > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        height > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return encoded;
    }
    stbi_write_png_to_func(
        append_encoded_bytes,
        &encoded,
        static_cast<int>(width),
        static_cast<int>(height),
        4,
        rgba.data(),
        static_cast<int>(width * 4));
    return encoded;
}

[[nodiscard]] std::vector<std::uint8_t> encode_jpeg(
    std::span<const std::uint8_t> rgba,
    std::size_t width,
    std::size_t height,
    int quality) {
    std::vector<std::uint8_t> rgb(width * height * 3);
    for (std::size_t source = 0, destination = 0; source < rgba.size(); source += 4) {
        rgb[destination++] = rgba[source];
        rgb[destination++] = rgba[source + 1];
        rgb[destination++] = rgba[source + 2];
    }
    std::vector<std::uint8_t> encoded;
    if (width > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        height > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return encoded;
    }
    stbi_write_jpg_to_func(
        append_encoded_bytes,
        &encoded,
        static_cast<int>(width),
        static_cast<int>(height),
        3,
        rgb.data(),
        quality);
    return encoded;
}

[[nodiscard]] ImageDimensions bounded_dimensions(
    ImageDimensions original,
    const ImageProcessingLimits& limits) {
    auto width = original.width;
    auto height = original.height;
    if (width > limits.max_width) {
        height = std::max<std::size_t>(
            1,
            static_cast<std::size_t>(std::llround(
                static_cast<double>(height) *
                static_cast<double>(limits.max_width) /
                static_cast<double>(width))));
        width = limits.max_width;
    }
    if (height > limits.max_height) {
        width = std::max<std::size_t>(
            1,
            static_cast<std::size_t>(std::llround(
                static_cast<double>(width) *
                static_cast<double>(limits.max_height) /
                static_cast<double>(height))));
        height = limits.max_height;
    }
    return {.width = width, .height = height};
}

[[nodiscard]] std::optional<std::vector<std::uint8_t>> resize_rgba(
    const DecodedImage& image,
    ImageDimensions target) {
    std::vector<std::uint8_t> resized(target.width * target.height * 4);
    const auto result = stbir_resize_uint8_linear(
        image.rgba.data(),
        static_cast<int>(image.dimensions.width),
        static_cast<int>(image.dimensions.height),
        0,
        resized.data(),
        static_cast<int>(target.width),
        static_cast<int>(target.height),
        0,
        STBIR_RGBA);
    if (result == nullptr) return std::nullopt;
    return resized;
}

[[nodiscard]] std::optional<ai::ImageContent> first_fitting_candidate(
    std::span<const std::uint8_t> rgba,
    ImageDimensions dimensions,
    std::size_t max_base64_bytes) {
    auto png = encode_png(rgba, dimensions.width, dimensions.height);
    auto png_base64 = encode_base64(png);
    if (!png.empty() && png_base64.size() < max_base64_bytes) {
        return ai::ImageContent{.data = std::move(png_base64), .mime_type = "image/png"};
    }

    constexpr std::array<int, 5> kJpegQualities{80, 85, 70, 55, 40};
    for (const auto quality : kJpegQualities) {
        auto jpeg = encode_jpeg(rgba, dimensions.width, dimensions.height, quality);
        auto jpeg_base64 = encode_base64(jpeg);
        if (!jpeg.empty() && jpeg_base64.size() < max_base64_bytes) {
            return ai::ImageContent{.data = std::move(jpeg_base64), .mime_type = "image/jpeg"};
        }
    }
    return std::nullopt;
}

} // namespace

std::optional<std::string> sniff_supported_image_mime_type(
    std::span<const std::uint8_t> bytes) {
    constexpr std::array<std::uint8_t, 8> kPng{0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
    constexpr std::array<std::uint8_t, 3> kJpeg{0xff, 0xd8, 0xff};
    constexpr std::array<std::uint8_t, 3> kGif{'G', 'I', 'F'};
    if (has_prefix(bytes, kPng) && is_static_png(bytes)) return "image/png";
    if (has_prefix(bytes, kJpeg) && (bytes.size() < 4 || bytes[3] != 0xf7)) return "image/jpeg";
    if (has_prefix(bytes, kGif)) return "image/gif";
    if (bytes_equal(bytes, 0, "RIFF") && bytes_equal(bytes, 8, "WEBP")) {
        return "image/webp";
    }
    return std::nullopt;
}

std::optional<std::string_view> extension_for_image_mime_type(
    std::string_view mime_type) {
    if (mime_type == "image/png") return ".png";
    if (mime_type == "image/jpeg" || mime_type == "image/jpg") return ".jpg";
    if (mime_type == "image/gif") return ".gif";
    if (mime_type == "image/webp") return ".webp";
    return std::nullopt;
}

support::Expected<ProcessedImageInput> process_image_input(
    std::span<const std::uint8_t> bytes,
    std::string mime_type,
    ImageProcessingLimits limits) {
    if (limits.max_width == 0 || limits.max_height == 0 || limits.max_base64_bytes == 0) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "image processing limits must be positive"));
    }
    if (mime_type == "image/jpg") mime_type = "image/jpeg";
    if (!extension_for_image_mime_type(mime_type)) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "unsupported image MIME type: " + mime_type));
    }

    const auto decoded = decode_image(bytes, mime_type);
    if (!decoded) {
        return ProcessedImageInput{
            .image = std::nullopt,
            .hints = {},
            .omission = std::string(kResizeOmission),
        };
    }

    auto encoded = encode_base64(bytes);
    if (decoded->dimensions.width <= limits.max_width &&
        decoded->dimensions.height <= limits.max_height &&
        encoded.size() < limits.max_base64_bytes) {
        return ProcessedImageInput{
            .image = ai::ImageContent{.data = std::move(encoded), .mime_type = std::move(mime_type)},
            .hints = {},
            .omission = std::nullopt,
        };
    }

    auto current = bounded_dimensions(decoded->dimensions, limits);
    while (true) {
        const auto resized = resize_rgba(*decoded, current);
        if (!resized) {
            return ProcessedImageInput{
                .image = std::nullopt,
                .hints = {},
                .omission = std::string(kResizeOmission),
            };
        }
        if (auto candidate = first_fitting_candidate(
                *resized,
                current,
                limits.max_base64_bytes)) {
            const auto scale = static_cast<double>(decoded->dimensions.width) /
                static_cast<double>(current.width);
            return ProcessedImageInput{
                .image = std::move(*candidate),
                .hints = {std::format(
                    "[Image: original {}x{}, displayed at {}x{}. Multiply coordinates by {:.2f} to map to original image.]",
                    decoded->dimensions.width,
                    decoded->dimensions.height,
                    current.width,
                    current.height,
                    scale)},
                .omission = std::nullopt,
            };
        }
        if (current.width == 1 && current.height == 1) break;
        const auto next_width = current.width == 1
            ? std::size_t{1}
            : std::max<std::size_t>(1, static_cast<std::size_t>(current.width * 0.75));
        const auto next_height = current.height == 1
            ? std::size_t{1}
            : std::max<std::size_t>(1, static_cast<std::size_t>(current.height * 0.75));
        if (next_width == current.width && next_height == current.height) break;
        current = {.width = next_width, .height = next_height};
    }

    return ProcessedImageInput{
        .image = std::nullopt,
        .hints = {},
        .omission = std::string(kResizeOmission),
    };
}

} // namespace cch::coding_agent

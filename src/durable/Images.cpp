#include <cch/durable/Images.hpp>

// debt: the pi-neutral decode/encode pipeline duplicates
// src/coding_agent/ImageInput.cpp because durable cannot depend on that Owner;
// consolidate into a shared support seam when a third consumer appears.

// Vendored stb headers sit outside the project warning contract (ADR 0044):
// diagnostics they trigger are silenced at this inclusion site. `*_STATIC`
// keeps durable's copies internal, so a final binary that also links the
// coding agent does not see duplicate stb symbols.
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#define STB_IMAGE_RESIZE_STATIC
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include <stb_image_resize2.h>
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#include <webp/decode.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <format>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace cch::durable {
namespace {

[[nodiscard]] bool has_prefix(std::span<const std::uint8_t> bytes, std::span<const std::uint8_t> prefix) {
    return std::ranges::starts_with(bytes, prefix);
}

[[nodiscard]] bool ascii_at(std::span<const std::uint8_t> bytes, std::size_t offset, std::string_view text) {
    if (offset > bytes.size() || text.size() > bytes.size() - offset) return false;
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (bytes[offset + index] != static_cast<std::uint8_t>(text[index])) return false;
    }
    return true;
}

[[nodiscard]] std::uint16_t read_u16_le(std::span<const std::uint8_t> bytes, std::size_t offset) {
    return static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(bytes[offset]) | (static_cast<std::uint16_t>(bytes[offset + 1]) << 8U));
}

[[nodiscard]] std::uint16_t read_u16_be(std::span<const std::uint8_t> bytes, std::size_t offset) {
    return static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(bytes[offset]) << 8U) | static_cast<std::uint16_t>(bytes[offset + 1]));
}

[[nodiscard]] std::uint32_t read_u32_be(std::span<const std::uint8_t> bytes, std::size_t offset) {
    return (static_cast<std::uint32_t>(bytes[offset]) << 24U) | (static_cast<std::uint32_t>(bytes[offset + 1]) << 16U) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 8U) | static_cast<std::uint32_t>(bytes[offset + 3]);
}

[[nodiscard]] std::uint32_t read_u32_le(std::span<const std::uint8_t> bytes, std::size_t offset) {
    return static_cast<std::uint32_t>(bytes[offset]) | (static_cast<std::uint32_t>(bytes[offset + 1]) << 8U) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16U) |
           (static_cast<std::uint32_t>(bytes[offset + 3]) << 24U);
}

[[nodiscard]] bool is_png(std::span<const std::uint8_t> bytes) {
    return bytes.size() >= 16 && read_u32_be(bytes, 8) == 13 && ascii_at(bytes, 12, "IHDR");
}

[[nodiscard]] bool is_animated_png(std::span<const std::uint8_t> bytes) {
    std::size_t offset = 8;
    while (offset + 8 <= bytes.size()) {
        const auto length = static_cast<std::size_t>(read_u32_be(bytes, offset));
        if (ascii_at(bytes, offset + 4, "acTL")) return true;
        if (ascii_at(bytes, offset + 4, "IDAT")) return false;
        if (length > bytes.size() - offset - 8) return false;
        offset += length + 12;
    }
    return false;
}

[[nodiscard]] bool is_bmp(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < 26) return false;
    const auto declared_file_size = read_u32_le(bytes, 2);
    const auto pixel_data_offset = read_u32_le(bytes, 10);
    const auto dib_header_size = read_u32_le(bytes, 14);
    if (declared_file_size != 0 && declared_file_size < 26) return false;
    if (pixel_data_offset < 14 + dib_header_size) return false;
    if (declared_file_size != 0 && pixel_data_offset >= declared_file_size) return false;

    std::uint16_t color_planes = 0;
    std::uint16_t bits_per_pixel = 0;
    if (dib_header_size == 12) {
        color_planes = read_u16_le(bytes, 22);
        bits_per_pixel = read_u16_le(bytes, 24);
    } else if (dib_header_size >= 40 && dib_header_size <= 124) {
        if (bytes.size() < 30) return false;
        color_planes = read_u16_le(bytes, 26);
        bits_per_pixel = read_u16_le(bytes, 28);
    } else {
        return false;
    }
    if (color_planes != 1) return false;
    return bits_per_pixel == 1 || bits_per_pixel == 4 || bits_per_pixel == 8 || bits_per_pixel == 16 ||
           bits_per_pixel == 24 || bits_per_pixel == 32;
}

} // namespace

bool is_inline_image_type(std::string_view mime_type) {
    return mime_type == "image/png" || mime_type == "image/jpeg" || mime_type == "image/gif" ||
           mime_type == "image/webp";
}

std::size_t base64_encoded_length(std::size_t byte_length) { return (byte_length + 2) / 3 * 4; }

std::optional<std::string> detect_image_mime_type(std::span<const std::uint8_t> bytes) {
    constexpr std::array<std::uint8_t, 3> kJpeg{0xff, 0xd8, 0xff};
    constexpr std::array<std::uint8_t, 8> kPng{0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
    constexpr std::array<std::uint8_t, 6> kGif87{'G', 'I', 'F', '8', '7', 'a'};
    constexpr std::array<std::uint8_t, 6> kGif89{'G', 'I', 'F', '8', '9', 'a'};
    constexpr std::array<std::uint8_t, 4> kRiff{'R', 'I', 'F', 'F'};
    constexpr std::array<std::uint8_t, 2> kBmp{'B', 'M'};
    if (has_prefix(bytes, kJpeg)) {
        return bytes.size() < 4 || bytes[3] != 0xf7 ? std::optional<std::string>{"image/jpeg"} : std::nullopt;
    }
    if (has_prefix(bytes, kPng)) {
        return is_png(bytes) && !is_animated_png(bytes) ? std::optional<std::string>{"image/png"} : std::nullopt;
    }
    if (has_prefix(bytes, kGif87) || has_prefix(bytes, kGif89)) return "image/gif";
    if (has_prefix(bytes, kRiff) && ascii_at(bytes, 8, "WEBP")) return "image/webp";
    if (has_prefix(bytes, kBmp) && is_bmp(bytes)) return "image/bmp";
    return std::nullopt;
}

support::Expected<ImageDimensions> image_dimensions(std::span<const std::uint8_t> bytes, std::string_view mime_type) {
    const auto unreadable = [&mime_type] {
        return std::unexpected(support::make_error(
                support::ErrorCode::Validation, std::format("{} header has no readable dimensions", mime_type)));
    };
    if (mime_type == "image/png") {
        if (bytes.size() < 24) return unreadable();
        return ImageDimensions{.width = read_u32_be(bytes, 16), .height = read_u32_be(bytes, 20)};
    }
    if (mime_type == "image/gif") {
        if (bytes.size() < 10) return unreadable();
        return ImageDimensions{.width = read_u16_le(bytes, 6), .height = read_u16_le(bytes, 8)};
    }
    if (mime_type == "image/webp") {
        if (bytes.size() < 30) return unreadable();
        if (ascii_at(bytes, 12, "VP8 ")) {
            return ImageDimensions{.width = static_cast<std::size_t>(read_u16_le(bytes, 26) & 0x3fffU),
                    .height = static_cast<std::size_t>(read_u16_le(bytes, 28) & 0x3fffU)};
        }
        if (ascii_at(bytes, 12, "VP8L")) {
            const auto bits = read_u32_le(bytes, 21);
            return ImageDimensions{.width = static_cast<std::size_t>(bits & 0x3fffU) + 1,
                    .height = static_cast<std::size_t>((bits >> 14U) & 0x3fffU) + 1};
        }
        if (ascii_at(bytes, 12, "VP8X")) {
            const auto uint24 = [bytes](std::size_t offset) {
                return static_cast<std::size_t>(read_u16_le(bytes, offset)) +
                       (static_cast<std::size_t>(bytes[offset + 2]) << 16U);
            };
            return ImageDimensions{.width = uint24(24) + 1, .height = uint24(27) + 1};
        }
        return unreadable();
    }
    if (mime_type == "image/jpeg") {
        for (std::size_t offset = 2; offset + 9 <= bytes.size();) {
            if (bytes[offset] != 0xff) return unreadable();
            const auto marker = bytes[offset + 1];
            if (marker == 0xff) {
                ++offset;
                continue;
            }
            if (marker >= 0xc0 && marker <= 0xcf && marker != 0xc4 && marker != 0xc8 && marker != 0xcc) {
                return ImageDimensions{
                        .width = read_u16_be(bytes, offset + 7), .height = read_u16_be(bytes, offset + 5)};
            }
            offset += 2 + read_u16_be(bytes, offset + 2);
        }
        return unreadable();
    }
    return unreadable();
}

namespace {

constexpr std::array<int, 4> kJpegQualities{80, 70, 55, 40};
constexpr std::string_view kBase64Alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

struct DecodedImage {
    std::vector<std::uint8_t> rgba;
    ImageDimensions dimensions;
    int orientation{1};
};

[[nodiscard]] bool is_exif_header(std::span<const std::uint8_t> bytes, std::size_t offset) {
    return ascii_at(bytes, offset, "Exif") && offset + 5 < bytes.size() && bytes[offset + 4] == 0 &&
           bytes[offset + 5] == 0;
}

/// Where the TIFF header of a JPEG's APP1 or a WebP's EXIF chunk starts;
/// `std::nullopt` when there is none.
[[nodiscard]] std::optional<std::size_t> tiff_start(std::span<const std::uint8_t> bytes) {
    constexpr std::array<std::uint8_t, 2> kJpegSoi{0xff, 0xd8};
    constexpr std::array<std::uint8_t, 4> kRiff{'R', 'I', 'F', 'F'};
    if (has_prefix(bytes, kJpegSoi)) {
        for (std::size_t offset = 2; offset + 4 <= bytes.size();) {
            if (bytes[offset] != 0xff) return std::nullopt;
            const auto marker = bytes[offset + 1];
            if (marker == 0xff) {
                ++offset;
                continue;
            }
            if (marker == 0xe1 && is_exif_header(bytes, offset + 4)) return offset + 10;
            offset += 2 + ((static_cast<std::size_t>(bytes[offset + 2]) << 8U) | bytes[offset + 3]);
        }
        return std::nullopt;
    }
    if (has_prefix(bytes, kRiff) && ascii_at(bytes, 8, "WEBP")) {
        for (std::size_t offset = 12; offset + 8 <= bytes.size();) {
            const auto size = static_cast<std::size_t>(read_u32_le(bytes, offset + 4));
            if (ascii_at(bytes, offset, "EXIF")) {
                return is_exif_header(bytes, offset + 8) ? offset + 14 : offset + 8;
            }
            if (size > bytes.size() - offset - 8) return std::nullopt;
            offset += 8 + size + (size % 2);
        }
    }
    return std::nullopt;
}

/// The EXIF orientation of a JPEG or WebP, 1 to 8; 1 when absent or unreadable.
[[nodiscard]] int exif_orientation(std::span<const std::uint8_t> bytes) {
    const auto start = tiff_start(bytes);
    if (!start || *start + 8 > bytes.size()) return 1;
    const auto tiff = *start;
    const bool little = bytes[tiff] == 0x49 && bytes[tiff + 1] == 0x49;
    const auto u16 = [bytes, little](std::size_t offset) {
        return little ? read_u16_le(bytes, offset) : read_u16_be(bytes, offset);
    };
    const auto u32 = [bytes, little](std::size_t offset) -> std::size_t {
        return little ? read_u32_le(bytes, offset) : read_u32_be(bytes, offset);
    };
    const auto directory = tiff + u32(tiff + 4);
    if (directory > bytes.size() || bytes.size() - directory < 2) return 1;
    const auto entries = static_cast<std::size_t>(u16(directory));
    for (std::size_t index = 0; index < entries; ++index) {
        const auto entry = directory + 2 + index * 12;
        if (entry + 12 > bytes.size()) return 1;
        if (u16(entry) == 0x0112) {
            const auto raw = u16(entry + 8);
            if (!std::in_range<int>(raw)) return 1;
            const auto value = static_cast<int>(raw);
            return value >= 1 && value <= 8 ? value : 1;
        }
    }
    return 1;
}

void apply_orientation(DecodedImage& image, int orientation) {
    if (orientation <= 1 || orientation > 8) return;
    const auto source_width = image.dimensions.width;
    const auto source_height = image.dimensions.height;
    const bool transposed = orientation >= 5;
    const auto output_width = transposed ? source_height : source_width;
    const auto output_height = transposed ? source_width : source_height;
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

[[nodiscard]] std::optional<DecodedImage> decode_image(
        std::span<const std::uint8_t> bytes, std::string_view mime_type) {
    int width = 0;
    int height = 0;
    std::uint8_t* decoded = nullptr;
    bool webp_allocation = false;
    if (mime_type == "image/webp") {
        decoded = WebPDecodeRGBA(bytes.data(), bytes.size(), &width, &height);
        webp_allocation = true;
    } else {
        if (bytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) return std::nullopt;
        int channels = 0;
        decoded = stbi_load_from_memory(
                bytes.data(), static_cast<int>(bytes.size()), &width, &height, &channels, STBI_rgb_alpha);
    }
    if (decoded == nullptr || width <= 0 || height <= 0) {
        if (decoded != nullptr) {
            if (webp_allocation)
                WebPFree(decoded);
            else
                stbi_image_free(decoded);
        }
        return std::nullopt;
    }
    const auto size = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4;
    DecodedImage image{.rgba = std::vector<std::uint8_t>(decoded, decoded + size),
            .dimensions = {.width = static_cast<std::size_t>(width), .height = static_cast<std::size_t>(height)}};
    if (webp_allocation)
        WebPFree(decoded);
    else
        stbi_image_free(decoded);
    if (mime_type == "image/jpeg" || mime_type == "image/webp") {
        image.orientation = exif_orientation(bytes);
        apply_orientation(image, image.orientation);
    }
    return image;
}

void append_encoded_bytes(void* context, void* data, int size) {
    if (context == nullptr || data == nullptr || size <= 0) return;
    auto& bytes = *static_cast<std::vector<std::uint8_t>*>(context);
    const auto* first = static_cast<const std::uint8_t*>(data);
    bytes.insert(bytes.end(), first, first + size);
}

[[nodiscard]] bool fits_stb_dimensions(ImageDimensions dimensions) {
    return std::in_range<int>(dimensions.width) && std::in_range<int>(dimensions.height);
}

[[nodiscard]] std::vector<std::uint8_t> encode_png(std::span<const std::uint8_t> rgba, ImageDimensions dimensions) {
    std::vector<std::uint8_t> encoded;
    if (!fits_stb_dimensions(dimensions) ||
            dimensions.width > static_cast<std::size_t>(std::numeric_limits<int>::max()) / 4)
        return encoded;
    stbi_write_png_to_func(append_encoded_bytes,
            &encoded,
            static_cast<int>(dimensions.width),
            static_cast<int>(dimensions.height),
            4,
            rgba.data(),
            static_cast<int>(dimensions.width * 4));
    return encoded;
}

[[nodiscard]] std::vector<std::uint8_t> encode_jpeg(
        std::span<const std::uint8_t> rgba, ImageDimensions dimensions, int quality) {
    if (!fits_stb_dimensions(dimensions)) return {};
    std::vector<std::uint8_t> rgb(dimensions.width * dimensions.height * 3);
    for (std::size_t source = 0, destination = 0; source < rgba.size(); source += 4) {
        rgb[destination++] = rgba[source];
        rgb[destination++] = rgba[source + 1];
        rgb[destination++] = rgba[source + 2];
    }
    std::vector<std::uint8_t> encoded;
    stbi_write_jpg_to_func(append_encoded_bytes,
            &encoded,
            static_cast<int>(dimensions.width),
            static_cast<int>(dimensions.height),
            3,
            rgb.data(),
            quality);
    return encoded;
}

struct EncodedImage {
    std::string data;
    std::string mime_type;
};

/// `image` at `dimensions` in the first encoding whose base64 fits
/// `max_base64_bytes`: PNG, then JPEG at falling qualities, or the JPEG
/// qualities first for a JPEG source.
[[nodiscard]] std::optional<EncodedImage> encode_within(
        std::span<const std::uint8_t> rgba, ImageDimensions dimensions, std::size_t max_base64_bytes, bool jpeg_first) {
    const auto fits = [max_base64_bytes](const std::vector<std::uint8_t>& encoded) {
        return !encoded.empty() && base64_encoded_length(encoded.size()) <= max_base64_bytes;
    };
    const auto jpegs_fit = [&]() -> std::optional<EncodedImage> {
        for (const auto quality : kJpegQualities) {
            auto encoded = encode_jpeg(rgba, dimensions, quality);
            if (fits(encoded)) return EncodedImage{.data = encode_base64(encoded), .mime_type = "image/jpeg"};
        }
        return std::nullopt;
    };
    const auto png_fits = [&]() -> std::optional<EncodedImage> {
        auto encoded = encode_png(rgba, dimensions);
        if (fits(encoded)) return EncodedImage{.data = encode_base64(encoded), .mime_type = "image/png"};
        return std::nullopt;
    };
    if (jpeg_first) {
        if (auto jpeg = jpegs_fit()) return jpeg;
        return png_fits();
    }
    if (auto png = png_fits()) return png;
    return jpegs_fit();
}

[[nodiscard]] std::optional<std::vector<std::uint8_t>> resize_rgba(const DecodedImage& image, ImageDimensions target) {
    if (!fits_stb_dimensions(image.dimensions) || !fits_stb_dimensions(target)) return std::nullopt;
    std::vector<std::uint8_t> resized(target.width * target.height * 4);
    const auto result = stbir_resize_uint8_linear(image.rgba.data(),
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

class LocalImageProcessor final : public ImageProcessor {
public:
    [[nodiscard]] support::Expected<PreparedImage> prepare(
            std::span<const std::uint8_t> bytes, std::string_view mime_type, const ImageLimits& limits) const override {
        const auto decoded = decode_image(bytes, mime_type);
        if (!decoded) {
            return std::unexpected(support::make_error(
                    support::ErrorCode::Validation, std::format("{} image cannot be decoded", mime_type)));
        }
        if (is_inline_image_type(mime_type) && decoded->orientation == 1 &&
                decoded->dimensions.width <= limits.max_width && decoded->dimensions.height <= limits.max_height &&
                base64_encoded_length(bytes.size()) <= limits.max_base64_bytes) {
            return PreparedImage{.data = encode_base64(bytes),
                    .mime_type = std::string{mime_type},
                    .resized = std::nullopt,
                    .converted_from = std::nullopt};
        }

        const double scale = std::min({1.0,
                limits.max_width == 0
                        ? 0.0
                        : static_cast<double>(limits.max_width) / static_cast<double>(decoded->dimensions.width),
                limits.max_height == 0
                        ? 0.0
                        : static_cast<double>(limits.max_height) / static_cast<double>(decoded->dimensions.height)});
        auto target = ImageDimensions{
                .width = std::max<std::size_t>(1,
                        static_cast<std::size_t>(std::llround(static_cast<double>(decoded->dimensions.width) * scale))),
                .height = std::max<std::size_t>(1,
                        static_cast<std::size_t>(
                                std::llround(static_cast<double>(decoded->dimensions.height) * scale)))};
        for (;;) {
            std::optional<std::vector<std::uint8_t>> resized;
            std::span<const std::uint8_t> pixels = decoded->rgba;
            if (target != decoded->dimensions) {
                resized = resize_rgba(*decoded, target);
                if (!resized) {
                    return std::unexpected(support::make_error(
                            support::ErrorCode::Validation, "image cannot be resized within its limits"));
                }
                pixels = std::span<const std::uint8_t>{*resized};
            }
            if (auto encoded = encode_within(pixels, target, limits.max_base64_bytes, mime_type == "image/jpeg")) {
                PreparedImage prepared{.data = std::move(encoded->data),
                        .mime_type = std::move(encoded->mime_type),
                        .resized = std::nullopt,
                        .converted_from = std::nullopt};
                if (target != decoded->dimensions) {
                    prepared.resized = PreparedImage::Resized{.from = decoded->dimensions, .to = target};
                }
                if (prepared.mime_type != mime_type) prepared.converted_from = std::string{mime_type};
                return prepared;
            }
            if (target.width == 1 && target.height == 1) {
                return std::unexpected(support::make_error(
                        support::ErrorCode::Validation, "image cannot be encoded within its byte limit"));
            }
            target = ImageDimensions{.width = std::max<std::size_t>(1, static_cast<std::size_t>(target.width * 0.75)),
                    .height = std::max<std::size_t>(1, static_cast<std::size_t>(target.height * 0.75))};
        }
    }
};

} // namespace

std::string encode_base64(std::span<const std::uint8_t> bytes) {
    std::string encoded;
    encoded.reserve(base64_encoded_length(bytes.size()));
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

std::unique_ptr<ImageProcessor> local_image_processor() { return std::make_unique<LocalImageProcessor>(); }

} // namespace cch::durable

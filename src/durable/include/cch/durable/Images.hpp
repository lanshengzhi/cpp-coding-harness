#pragma once

#include <cch/support/Error.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace cch::durable {

/// Pixel dimensions declared in an image header.
struct ImageDimensions {
    std::size_t width{};
    std::size_t height{};

    [[nodiscard]] bool operator==(const ImageDimensions&) const = default;
};

/// Limits an image the model sees must keep. Defaults are the frozen
/// coding-agent limits: 4.5 MiB of base64, below Anthropic's 5 MB.
struct ImageLimits {
    std::size_t max_width{2000};
    std::size_t max_height{2000};
    std::size_t max_base64_bytes{4'718'592};
};

/// An image the model can take: a supported format, within the limits, as
/// base64. `resized` describes the upright dimensions before and after
/// scaling; `converted_from` is set when the encoded format differs.
struct PreparedImage {
    struct Resized {
        ImageDimensions from{};
        ImageDimensions to{};
    };

    std::string data{};
    std::string mime_type{};
    std::optional<Resized> resized{};
    std::optional<std::string> converted_from{};
};

/// Turns bytes into an image the model can take. A processor that cannot
/// decode or fit the image returns an error, which the read tool reports as an
/// image that cannot be prepared.
class ImageProcessor {
public:
    virtual ~ImageProcessor() = default;
    [[nodiscard]] virtual support::Expected<PreparedImage> prepare(
            std::span<const std::uint8_t> bytes, std::string_view mime_type, const ImageLimits& limits) const = 0;
};

/// The durable runtime's own processor: stb decodes, orients by EXIF,
/// resizes, and re-encodes within the limits.
[[nodiscard]] std::unique_ptr<ImageProcessor> local_image_processor();

/// Whether providers take images of this MIME type inline. BMP and unknown
/// types need conversion.
[[nodiscard]] bool is_inline_image_type(std::string_view mime_type);

/// The standard base64 length of `byte_length` bytes, without encoding them.
[[nodiscard]] std::size_t base64_encoded_length(std::size_t byte_length);

/// The MIME type of an image the read tool recognizes, or null when the bytes
/// are not one: PNG must be static, JPEG byte 3 `0xf7` is rejected, GIF needs
/// `GIF87a`/`GIF89a`, and BMP is validated as the frozen reader does.
[[nodiscard]] std::optional<std::string> detect_image_mime_type(std::span<const std::uint8_t> bytes);

/// Standard base64 of `bytes`, used for image blocks.
[[nodiscard]] std::string encode_base64(std::span<const std::uint8_t> bytes);

/// The width and height an image of `mime_type` declares in its header,
/// without decoding it. BMP has no frozen header-only reader.
[[nodiscard]] support::Expected<ImageDimensions> image_dimensions(
        std::span<const std::uint8_t> bytes, std::string_view mime_type);

} // namespace cch::durable

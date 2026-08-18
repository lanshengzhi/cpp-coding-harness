#include <cch/tui/Image.hpp>

#include <cch/tui/TerminalImage.hpp>
#include <cch/tui/Utils.hpp>

#include "tui/UnicodeWidth.hpp"

#include <cch/support/Error.hpp>
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Behavioral baseline: pi 83114817 packages/tui/src/components/image.ts and
// packages/tui/src/terminal-image.ts. The Image component reports sniffed
// dimensions, renders the pi-exact `[Image: path [mime] WxH]` fallback
// (imageFallback with ~/ shortening and file:// linking gated on the
// hyperlinks capability), and truncates the fallback to the render width
// with truncateToWidth's "..." ellipsis. Native sizing and protocol selection
// are terminal-owned under the sidecar model (fork B); resource-id/revision
// reuse keeps animation frames at the same placement.

namespace cch::tui {
namespace {

struct ImageDimensions {
    std::size_t width{0};
    std::size_t height{0};
};

constexpr std::size_t kMaxDecodedBytes = 64U * 1024U * 1024U;

std::atomic<std::uint64_t> g_next_image_id{1};

[[nodiscard]] std::optional<std::uint8_t> base64_value(char value) {
    if (value >= 'A' && value <= 'Z') return static_cast<std::uint8_t>(value - 'A');
    if (value >= 'a' && value <= 'z') return static_cast<std::uint8_t>(value - 'a' + 26);
    if (value >= '0' && value <= '9') return static_cast<std::uint8_t>(value - '0' + 52);
    if (value == '+') return 62;
    if (value == '/') return 63;
    return std::nullopt;
}

[[nodiscard]] std::optional<std::vector<std::uint8_t>> decode_base64(std::string_view encoded) {
    if (encoded.empty() || encoded.size() % 4 != 0 ||
        encoded.size() > ((kMaxDecodedBytes + 2U) / 3U) * 4U) {
        return std::nullopt;
    }
    const auto padding = encoded.ends_with("==") ? 2U : encoded.ends_with("=") ? 1U : 0U;
    const auto decoded_size = (encoded.size() / 4U) * 3U - padding;
    if (decoded_size == 0 || decoded_size > kMaxDecodedBytes) return std::nullopt;

    std::vector<std::uint8_t> decoded;
    decoded.reserve(decoded_size);
    for (std::size_t offset = 0; offset < encoded.size(); offset += 4) {
        std::uint32_t value = 0;
        for (std::size_t index = 0; index < 4; ++index) {
            const auto character = encoded[offset + index];
            const auto is_padding = character == '=';
            if (is_padding) {
                if (offset + 4 != encoded.size() || index < 2) return std::nullopt;
                value <<= 6U;
                continue;
            }
            if (offset + 4 == encoded.size() && padding != 0 && index >= 4 - padding) {
                return std::nullopt;
            }
            const auto decoded_value = base64_value(character);
            if (!decoded_value) return std::nullopt;
            value = (value << 6U) | *decoded_value;
        }
        decoded.push_back(static_cast<std::uint8_t>(value >> 16U));
        if (decoded.size() < decoded_size) decoded.push_back(static_cast<std::uint8_t>(value >> 8U));
        if (decoded.size() < decoded_size) decoded.push_back(static_cast<std::uint8_t>(value));
    }
    return decoded;
}

[[nodiscard]] std::uint16_t read_be16(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(bytes[offset]) << 8U) |
        static_cast<std::uint16_t>(bytes[offset + 1]));
}

[[nodiscard]] std::uint32_t read_be32(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    return (static_cast<std::uint32_t>(bytes[offset]) << 24U) |
        (static_cast<std::uint32_t>(bytes[offset + 1]) << 16U) |
        (static_cast<std::uint32_t>(bytes[offset + 2]) << 8U) |
        static_cast<std::uint32_t>(bytes[offset + 3]);
}

[[nodiscard]] std::uint16_t read_le16(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(bytes[offset]) |
        (static_cast<std::uint16_t>(bytes[offset + 1]) << 8U));
}

[[nodiscard]] std::uint32_t read_le32(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    return static_cast<std::uint32_t>(bytes[offset]) |
        (static_cast<std::uint32_t>(bytes[offset + 1]) << 8U) |
        (static_cast<std::uint32_t>(bytes[offset + 2]) << 16U) |
        (static_cast<std::uint32_t>(bytes[offset + 3]) << 24U);
}

[[nodiscard]] bool bytes_equal(
    const std::vector<std::uint8_t>& bytes,
    std::size_t offset,
    std::string_view expected) {
    if (offset > bytes.size() || expected.size() > bytes.size() - offset) return false;
    for (std::size_t index = 0; index < expected.size(); ++index) {
        if (bytes[offset + index] != static_cast<std::uint8_t>(expected[index])) return false;
    }
    return true;
}

[[nodiscard]] std::optional<ImageDimensions> png_dimensions(const std::vector<std::uint8_t>& bytes) {
    constexpr std::uint8_t kSignature[]{0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a};
    if (bytes.size() < 8) return std::nullopt;
    for (std::size_t index = 0; index < 8; ++index) {
        if (bytes[index] != kSignature[index]) return std::nullopt;
    }

    std::optional<ImageDimensions> dimensions;
    std::size_t offset = 8;
    bool first_chunk = true;
    while (offset < bytes.size()) {
        if (bytes.size() - offset < 12) return std::nullopt;
        const auto chunk_size = static_cast<std::size_t>(read_be32(bytes, offset));
        if (chunk_size > bytes.size() - offset - 12) return std::nullopt;
        const auto chunk_end = offset + 12 + chunk_size;

        if (first_chunk) {
            if (!bytes_equal(bytes, offset + 4, "IHDR") || chunk_size != 13) {
                return std::nullopt;
            }
            const auto width = read_be32(bytes, offset + 8);
            const auto height = read_be32(bytes, offset + 12);
            if (width == 0 || height == 0) return std::nullopt;
            dimensions = ImageDimensions{.width = width, .height = height};
            first_chunk = false;
        } else if (bytes_equal(bytes, offset + 4, "IHDR")) {
            return std::nullopt;
        }

        if (bytes_equal(bytes, offset + 4, "IEND")) {
            if (chunk_size != 0 || chunk_end != bytes.size()) return std::nullopt;
            return dimensions;
        }
        offset = chunk_end;
    }
    return std::nullopt;
}

[[nodiscard]] bool is_sof_marker(std::uint8_t marker) {
    return marker >= 0xc0 && marker <= 0xcf &&
        marker != 0xc4 && marker != 0xc8 && marker != 0xcc;
}

[[nodiscard]] std::optional<ImageDimensions> jpeg_dimensions(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() < 4 || bytes[0] != 0xff || bytes[1] != 0xd8) return std::nullopt;
    std::optional<ImageDimensions> dimensions;
    std::size_t offset = 2;
    while (offset < bytes.size()) {
        if (bytes[offset] != 0xff) {
            ++offset;
            continue;
        }
        while (offset < bytes.size() && bytes[offset] == 0xff) ++offset;
        if (offset >= bytes.size()) return std::nullopt;
        const auto marker = bytes[offset++];
        if (marker == 0x00) continue;
        if (marker == 0xd9) return dimensions;
        if (marker == 0x01 || marker == 0xd8 ||
            (marker >= 0xd0 && marker <= 0xd7)) {
            continue;
        }
        if (bytes.size() - offset < 2) return std::nullopt;
        const auto length = static_cast<std::size_t>(read_be16(bytes, offset));
        if (length < 2 || length > bytes.size() - offset) return std::nullopt;
        if (is_sof_marker(marker)) {
            if (length < 7) return std::nullopt;
            const auto height = read_be16(bytes, offset + 3);
            const auto width = read_be16(bytes, offset + 5);
            if (width == 0 || height == 0) return std::nullopt;
            dimensions = ImageDimensions{.width = width, .height = height};
        }
        offset += length;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<ImageDimensions> gif_dimensions(const std::vector<std::uint8_t>& bytes) {
    constexpr std::size_t kLogicalScreenEnd = 13;
    if (bytes.size() <= kLogicalScreenEnd ||
        (!bytes_equal(bytes, 0, "GIF87a") && !bytes_equal(bytes, 0, "GIF89a")) ||
        bytes.back() != 0x3b) {
        return std::nullopt;
    }
    const auto width = read_le16(bytes, 6);
    const auto height = read_le16(bytes, 8);
    if (width == 0 || height == 0) return std::nullopt;

    if ((bytes[10] & 0x80U) != 0) {
        const auto color_count = std::size_t{2} << (bytes[10] & 0x07U);
        const auto color_table_bytes = color_count * 3U;
        if (color_table_bytes > bytes.size() - kLogicalScreenEnd - 1) return std::nullopt;
    }
    return ImageDimensions{.width = width, .height = height};
}

[[nodiscard]] std::optional<ImageDimensions> webp_dimensions(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() < 20 || !bytes_equal(bytes, 0, "RIFF") || !bytes_equal(bytes, 8, "WEBP")) {
        return std::nullopt;
    }
    const auto riff_size = static_cast<std::size_t>(read_le32(bytes, 4));
    if (riff_size != bytes.size() - 8) return std::nullopt;
    const auto chunk_size = static_cast<std::size_t>(read_le32(bytes, 16));
    const auto padded_chunk_size = chunk_size + (chunk_size & 1U);
    if (padded_chunk_size > bytes.size() - 20) return std::nullopt;

    if (bytes_equal(bytes, 12, "VP8X")) {
        if (chunk_size < 10) return std::nullopt;
        const auto width = static_cast<std::uint32_t>(bytes[24]) |
            (static_cast<std::uint32_t>(bytes[25]) << 8U) |
            (static_cast<std::uint32_t>(bytes[26]) << 16U);
        const auto height = static_cast<std::uint32_t>(bytes[27]) |
            (static_cast<std::uint32_t>(bytes[28]) << 8U) |
            (static_cast<std::uint32_t>(bytes[29]) << 16U);
        return ImageDimensions{.width = width + 1U, .height = height + 1U};
    }
    if (bytes_equal(bytes, 12, "VP8L")) {
        if (chunk_size < 5 || bytes[20] != 0x2f) return std::nullopt;
        const auto bits = read_le32(bytes, 21);
        return ImageDimensions{
            .width = (bits & 0x3fffU) + 1U,
            .height = ((bits >> 14U) & 0x3fffU) + 1U,
        };
    }
    if (bytes_equal(bytes, 12, "VP8 ")) {
        if (chunk_size < 10 || bytes[23] != 0x9d || bytes[24] != 0x01 || bytes[25] != 0x2a) {
            return std::nullopt;
        }
        const auto width = read_le16(bytes, 26) & 0x3fffU;
        const auto height = read_le16(bytes, 28) & 0x3fffU;
        if (width == 0 || height == 0) return std::nullopt;
        return ImageDimensions{.width = width, .height = height};
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<ImageDimensions> image_dimensions(
    const std::vector<std::uint8_t>& bytes,
    std::string_view mime_type) {
    if (mime_type == "image/png") return png_dimensions(bytes);
    if (mime_type == "image/jpeg") return jpeg_dimensions(bytes);
    if (mime_type == "image/gif") return gif_dimensions(bytes);
    if (mime_type == "image/webp") return webp_dimensions(bytes);
    return std::nullopt;
}

[[nodiscard]] std::string safe_label(std::string_view value) {
    std::string result;
    result.reserve(std::min<std::size_t>(value.size(), 128));
    for (const auto character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte < 0x20 || byte == 0x7f || character == '\x1b') result.push_back('?');
        else result.push_back(character);
        if (result.size() == 128) break;
    }
    return result;
}

} // namespace

struct Image::Impl {
    ImageContent content;
    ImageOptions options;
    std::uint64_t resource_id{g_next_image_id.fetch_add(1)};
    std::uint64_t revision{1};
    std::size_t cached_width{0};
    RenderResult cached;
    bool cache_valid{false};
    bool present{true};
};

Image::Image(ImageContent content, ImageOptions options)
    : impl_(std::make_unique<Impl>()) {
    impl_->content = std::move(content);
    impl_->options = std::move(options);
}

Image::Image(Image&&) noexcept = default;
Image& Image::operator=(Image&&) noexcept = default;
Image::~Image() = default;

void Image::set_content(ImageContent content) {
    impl_->content = std::move(content);
    impl_->present = true;
    ++impl_->revision;
    invalidate();
}

void Image::clear() {
    impl_->content = {};
    impl_->present = false;
    ++impl_->revision;
    invalidate();
}

bool Image::has_content() const {
    return impl_->present;
}

support::Expected<RenderResult> Image::render(std::size_t width) {
    if (width == 0) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "TUI Image requires a positive visible width"));
    }
    if (!impl_->present) return RenderResult{};
    if (impl_->cache_valid && impl_->cached_width == width) return impl_->cached;

    const auto decoded = decode_base64(impl_->content.encoded_data);
    const auto dimensions = decoded
        ? image_dimensions(*decoded, impl_->content.mime_type)
        : std::nullopt;
    const auto mime_type = safe_label(impl_->content.mime_type);
    std::optional<std::string_view> filename;
    std::string sanitized_filename;
    if (impl_->content.filename) {
        sanitized_filename = safe_label(*impl_->content.filename);
        filename = sanitized_filename;
    }
    // Pi-exact fallback text: `[Image: path [mime] WxH]` with ~/ shortening
    // and file:// linking when hyperlinks are available (pi imageFallback).
    // The C++ sanitization of control bytes and the 128-byte label cap stay
    // as a bounded hardening divergence; pi's Image component defaults
    // unsniffable dimensions to 800x600 (components/image.ts), so the WxH
    // segment is always present, exactly as in pi.
    auto fallback = image_fallback(
        mime_type,
        dimensions
            ? std::optional<ImagePixelSize>{ImagePixelSize{
                  .width = dimensions->width,
                  .height = dimensions->height,
              }}
            : std::optional<ImagePixelSize>{ImagePixelSize{.width = 800, .height = 600}},
        filename);

    // Pi's Image truncates the fallback to the render width (truncateToWidth
    // with its "..." ellipsis); the cell constraints apply to the native
    // image sizing only, never to the fallback line.
    auto truncated = truncate_text(fallback, width);
    if (!truncated) return std::unexpected(truncated.error());
    fallback = std::move(*truncated);

    if (impl_->options.fallback_style) {
        const auto original_width = visible_width(fallback);
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
        try {
#endif
            fallback = impl_->options.fallback_style(std::move(fallback));
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
        } catch (const std::exception&) {
            return std::unexpected(support::make_error(
                support::ErrorCode::Unknown,
                "TUI Image fallback style failed",
                "the style callback threw an exception"));
        } catch (...) {
            return std::unexpected(support::make_error(
                support::ErrorCode::Unknown,
                "TUI Image fallback style failed",
                "the style callback threw an unknown exception"));
        }
#endif
        if (visible_width(fallback) != original_width) {
            return std::unexpected(support::make_error(
                support::ErrorCode::Validation,
                "TUI Image fallback style changed visible width"));
        }
    }
    auto prepared = detail::prepare_rendered_line(fallback, width);
    if (!prepared) return std::unexpected(prepared.error());

    RenderResult output{.lines = {*prepared}};
    if (dimensions) {
        output.images.push_back({
            .resource_id = impl_->resource_id,
            .revision = impl_->revision,
            .encoded_data = impl_->content.encoded_data,
            .mime_type = impl_->content.mime_type,
            .filename = impl_->content.filename,
            .pixel_width = dimensions->width,
            .pixel_height = dimensions->height,
            .max_width = impl_->options.constraints.max_width,
            .max_height = impl_->options.constraints.max_height,
            .fallback_text = *prepared,
            .region = {.column = 0, .row = 0, .columns = 1, .rows = 1},
        });
    }

    impl_->cached_width = width;
    impl_->cached = output;
    impl_->cache_valid = true;
    return output;
}

void Image::invalidate() {
    impl_->cache_valid = false;
    impl_->cached = {};
}

} // namespace cch::tui

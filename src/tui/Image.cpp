#include <cch/tui/Image.hpp>

#include <cch/tui/TerminalImage.hpp>
#include <cch/tui/Utils.hpp>

#include "tui/UnicodeWidth.hpp"

#include <cch/support/Error.hpp>
#include <algorithm>
#include <atomic>
#include <cmath>
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

namespace {

std::atomic<std::uint64_t> g_next_image_id{1};

} // namespace

struct Image::Impl {
    ImageContent content;
    ImageOptions options;
    std::uint64_t resource_id{g_next_image_id.fetch_add(1)};
    std::uint64_t revision{1};
    std::size_t cached_width{0};
    DetectedImageCapabilities cached_capabilities{};
    CellPixelDimensions cached_cell_dimensions{};
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

    const auto capabilities = get_image_capabilities();
    const auto cell_dimensions = get_cell_dimensions();
    if (impl_->cache_valid && impl_->cached_width == width && impl_->cached_capabilities == capabilities &&
            impl_->cached_cell_dimensions == cell_dimensions) {
        return impl_->cached;
    }

    const auto detected_dimensions =
            impl_->options.dimensions ? impl_->options.dimensions
                                      : get_image_dimensions(impl_->content.encoded_data, impl_->content.mime_type);
    const auto dimensions = detected_dimensions.value_or(ImagePixelSize{.width = 800, .height = 600});
    const auto mime_type = safe_label(impl_->content.mime_type);
    std::optional<std::string_view> filename;
    std::string sanitized_filename;
    if (impl_->content.filename) {
        sanitized_filename = safe_label(*impl_->content.filename);
        filename = sanitized_filename;
    }
    auto fallback = image_fallback(mime_type, dimensions, filename);

    // Pi's Image truncates the fallback to the render width (truncateToWidth
    // with its "..." ellipsis); the cell constraints apply to the native
    // image sizing only, never to the fallback line.
    auto truncated = truncate_text(fallback, width);
    if (!truncated) return std::unexpected(truncated.error());
    fallback = std::move(*truncated);

    if (impl_->options.fallback_style) {
        const auto original_width = visible_width(fallback);
        fallback = impl_->options.fallback_style(std::move(fallback));
        if (visible_width(fallback) != original_width) {
            return std::unexpected(support::make_error(
                support::ErrorCode::Validation,
                "TUI Image fallback style changed visible width"));
        }
    }
    auto prepared = detail::prepare_rendered_line(fallback, width);
    if (!prepared) return std::unexpected(prepared.error());

    RenderResult output{.lines = {prepared->text}};
    const auto available_width = std::max<std::size_t>(1, width > 2 ? width - 2 : 1);
    const auto max_width =
            std::max<std::size_t>(1, std::min(available_width, impl_->options.constraints.max_width.value_or(60)));
    const auto cell_width = std::max<std::size_t>(1, cell_dimensions.width);
    const auto cell_height = std::max<std::size_t>(1, cell_dimensions.height);
    const auto default_max_height = std::max<std::size_t>(
            1, static_cast<std::size_t>(std::ceil(static_cast<long double>(max_width) * cell_width / cell_height)));
    const auto max_height = impl_->options.constraints.max_height.value_or(default_max_height);
    const auto default_geometry = calculate_image_cell_size(
            dimensions, max_width, max_height, cell_dimensions, capabilities.images == InlineImageProtocol::Kitty);
    if (capabilities.images != InlineImageProtocol::None &&
            detail::protocol_supports_mime(capabilities.images, impl_->content.mime_type)) {
        output.images.push_back({
                .resource_id = impl_->resource_id,
                .revision = impl_->revision,
                .encoded_data = impl_->content.encoded_data,
                .mime_type = impl_->content.mime_type,
                .filename = impl_->content.filename,
                .pixel_width = dimensions.width,
                .pixel_height = dimensions.height,
                .max_width = max_width,
                .max_height = max_height,
                .fallback_text = prepared->text,
                .region =
                        {
                                .column = 0,
                                .row = 0,
                                .columns = default_geometry.columns,
                                .rows = default_geometry.rows,
                        },
                .rows_reserved_in_lines = true,
        });
        output.lines.assign(default_geometry.rows, std::string{});
    }

    impl_->cached_width = width;
    impl_->cached_capabilities = capabilities;
    impl_->cached_cell_dimensions = cell_dimensions;
    impl_->cached = output;
    impl_->cache_valid = true;
    return output;
}

void Image::invalidate() {
    impl_->cache_valid = false;
    impl_->cached = {};
}

} // namespace cch::tui

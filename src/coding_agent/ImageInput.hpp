#pragma once

#include <cch/ai/Content.hpp>
#include <cch/support/Error.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cch::coding_agent {

struct ImageProcessingLimits {
    std::size_t max_width{2000};
    std::size_t max_height{2000};
    std::size_t max_base64_bytes{4'718'592};
};

struct ProcessedImageInput {
    std::optional<ai::ImageContent> image{std::nullopt};
    std::vector<std::string> hints;
    std::optional<std::string> omission{std::nullopt};
};

[[nodiscard]] std::optional<std::string> sniff_supported_image_mime_type(
    std::span<const std::uint8_t> bytes);
[[nodiscard]] std::optional<std::string_view> extension_for_image_mime_type(
    std::string_view mime_type);
[[nodiscard]] support::Expected<ProcessedImageInput> process_image_input(
    std::span<const std::uint8_t> bytes,
    std::string mime_type,
    ImageProcessingLimits limits = {});

} // namespace cch::coding_agent

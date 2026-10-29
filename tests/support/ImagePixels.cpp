#include "support/ImagePixels.hpp"

// Vendored stb is outside the project warning contract (ADR 0044); this
// `*_STATIC` copy stays internal to this test translation unit.
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <limits>

namespace cch::tests {

std::optional<std::vector<std::uint8_t>> decode_rgba(std::span<const std::uint8_t> bytes) {
    if (bytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) return std::nullopt;
    int width = 0;
    int height = 0;
    int channels = 0;
    auto* decoded = stbi_load_from_memory(
            bytes.data(), static_cast<int>(bytes.size()), &width, &height, &channels, STBI_rgb_alpha);
    if (decoded == nullptr || width <= 0 || height <= 0) {
        if (decoded != nullptr) stbi_image_free(decoded);
        return std::nullopt;
    }
    const auto size = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4;
    std::vector<std::uint8_t> pixels(decoded, decoded + size);
    stbi_image_free(decoded);
    return pixels;
}

} // namespace cch::tests

#include <cch/tui/TerminalImage.hpp>

#include <mutex>
#include <optional>
#include <utility>

namespace cch::tui {
namespace {

std::mutex g_image_state_mutex;
std::optional<DetectedImageCapabilities> g_image_capabilities;
ImageCapabilityOverrides g_image_capability_overrides;
CellPixelDimensions g_cell_dimensions{};

} // namespace

DetectedImageCapabilities get_image_capabilities() {
    std::lock_guard lock(g_image_state_mutex);
    if (!g_image_capabilities) {
        auto detected = detect_image_capabilities();
        if (g_image_capability_overrides.images) detected.images = *g_image_capability_overrides.images;
        if (g_image_capability_overrides.color) detected.color = *g_image_capability_overrides.color;
        if (g_image_capability_overrides.hyperlinks) detected.hyperlinks = *g_image_capability_overrides.hyperlinks;
        g_image_capabilities = detected;
    }
    return *g_image_capabilities;
}

void set_image_capabilities(DetectedImageCapabilities capabilities) {
    std::lock_guard lock(g_image_state_mutex);
    g_image_capabilities = capabilities;
}

void set_image_capability_overrides(ImageCapabilityOverrides overrides) {
    std::lock_guard lock(g_image_state_mutex);
    g_image_capability_overrides = std::move(overrides);
    g_image_capabilities.reset();
}

void reset_image_capability_overrides() { set_image_capability_overrides({}); }

void reset_image_capabilities_cache() {
    std::lock_guard lock(g_image_state_mutex);
    g_image_capabilities.reset();
}

CellPixelDimensions get_cell_dimensions() {
    std::lock_guard lock(g_image_state_mutex);
    return g_cell_dimensions;
}

void set_cell_dimensions(CellPixelDimensions dimensions) {
    if (dimensions.width == 0 || dimensions.height == 0) return;
    std::lock_guard lock(g_image_state_mutex);
    g_cell_dimensions = dimensions;
}

void reset_cell_dimensions() {
    std::lock_guard lock(g_image_state_mutex);
    g_cell_dimensions = {};
}

} // namespace cch::tui

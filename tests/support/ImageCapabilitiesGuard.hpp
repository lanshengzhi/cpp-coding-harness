#pragma once

#include <cch/tui/TerminalImage.hpp>

namespace cch::tests {

class ImageCapabilitiesGuard final {
public:
    explicit ImageCapabilitiesGuard(tui::DetectedImageCapabilities capabilities) {
        tui::set_image_capabilities(capabilities);
    }

    ImageCapabilitiesGuard(ImageCapabilitiesGuard&&) = delete;
    ImageCapabilitiesGuard& operator=(ImageCapabilitiesGuard&&) = delete;

    ~ImageCapabilitiesGuard() {
        tui::reset_image_capabilities_cache();
    }

    ImageCapabilitiesGuard(const ImageCapabilitiesGuard&) = delete;
    ImageCapabilitiesGuard& operator=(const ImageCapabilitiesGuard&) = delete;
};

} // namespace cch::tests

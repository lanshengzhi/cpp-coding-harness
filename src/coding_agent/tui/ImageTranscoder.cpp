#include "coding_agent/tui/ImageTranscoder.hpp"

#include "coding_agent/ImageInput.hpp"

#include <string>
#include <string_view>

namespace cch::coding_agent::tui {
namespace {

/// pi `pngTranscoderRegistered`: the host registers its decoder once, so a
/// later call must not reload or re-register it.
bool png_transcoder_registered{false};

} // namespace

cch::tui::ImageTranscoder load_png_transcoder() {
    return [](std::string_view base64_data, std::string_view mime_type) {
        return transcode_image_to_png(base64_data, mime_type);
    };
}

void ensure_png_transcoder(ImageTranscoderRegisteredSink on_registered) {
    if (png_transcoder_registered ||
            cch::tui::get_image_capabilities().images != cch::tui::InlineImageProtocol::Kitty) {
        return;
    }
    cch::tui::set_image_transcoder(load_png_transcoder());
    png_transcoder_registered = true;
    if (on_registered) on_registered();
}

} // namespace cch::coding_agent::tui

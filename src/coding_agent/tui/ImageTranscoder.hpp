#pragma once

#include <cch/tui/TerminalImage.hpp>

#include <functional>

namespace cch::coding_agent::tui {

/// Host hook that runs after a successful registration so callers can re-render
/// images that already painted their text fallback.
using ImageTranscoderRegisteredSink = std::move_only_function<void()>;

/// The product's synchronous converter for Kitty's PNG-only graphics protocol:
/// the pinned stb pipeline decodes the base64 payload (EXIF orientation
/// included) and re-encodes it as base64 PNG. Pi loads its decoder module here
/// (`loadPhoton`) and can report it unavailable; this binary links the decoder,
/// so loading always yields a converter and per-payload failures are the
/// converter's null result.
[[nodiscard]] cch::tui::ImageTranscoder load_png_transcoder();

/// Register `load_png_transcoder()` as the toolkit's conversion capability when
/// the detected terminal speaks Kitty, running `on_registered` on the call that
/// registers it. Non-Kitty terminals and later calls are no-ops (pi
/// `ensurePngTranscoder`).
void ensure_png_transcoder(ImageTranscoderRegisteredSink on_registered = {});

} // namespace cch::coding_agent::tui

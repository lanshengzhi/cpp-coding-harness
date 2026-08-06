#pragma once

#include <cch/tui/Terminal.hpp>
#include <cch/util/Error.hpp>

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Behavioral baseline: pi 83114817 packages/tui/src/terminal-image.ts
// (detectCapabilities env rules with the tmux probe, hyperlink,
// imageFallback with ~/ shortening and file:// linking, and the Kitty/iTerm2
// encoders). The sidecar model keeps placement terminal-owned; the detection,
// fallback, and encoder helpers are the pi-aligned public surface.

namespace cch::tui {

/// Pixel dimensions of an image (pi terminal-image.ts `ImageDimensions`).
struct ImagePixelSize {
    std::size_t width{0};
    std::size_t height{0};

    bool operator==(const ImagePixelSize&) const = default;
};

/// Env-rule terminal capability detection result (pi terminal-image.ts
/// `TerminalCapabilities` minus `trueColor`, which the terminal seam's color
/// capability already carries from the same env rules).
struct DetectedImageCapabilities {
    InlineImageProtocol images{InlineImageProtocol::None};
    bool hyperlinks{false};

    bool operator==(const DetectedImageCapabilities&) const = default;
};

/// Reports whether the attached tmux client forwards OSC 8 hyperlinks.
using TmuxHyperlinkProbe = std::move_only_function<bool()>;

namespace detail {

/// Default tmux probe implementation: runs
/// `tmux display-message -p '#{client_termfeatures}'` with pi's 250 ms
/// timeout and reports whether the comma-separated features list `hyperlinks`;
/// false on any failure or on platforms without process spawning.
[[nodiscard]] bool probe_tmux_hyperlinks();

} // namespace detail

/// Per-emulator env-rule capability detection exactly as pi's
/// `detectCapabilities`: tmux and screen disable inline images (tmux probes
/// OSC 8 forwarding), Kitty/Ghostty/WezTerm/Warp report the Kitty protocol,
/// iTerm2 reports the iTerm2 protocol, and every other emulator stays
/// conservative with no inline images.
[[nodiscard]] DetectedImageCapabilities detect_image_capabilities(
    TmuxHyperlinkProbe tmux_forwards_hyperlink = detail::probe_tmux_hyperlinks);

/// Cached `detect_image_capabilities()` result with test overrides, mirroring
/// pi's `getCapabilities`/`setCapabilities`/`resetCapabilitiesCache`.
[[nodiscard]] DetectedImageCapabilities get_image_capabilities();
void set_image_capabilities(DetectedImageCapabilities capabilities);
void reset_image_capabilities_cache();

/// Wrap `text` in an OSC 8 hyperlink sequence, exactly as pi's `hyperlink`
/// (`ESC ] 8 ;; url ESC \ text ESC ] 8 ;; ESC \`).
[[nodiscard]] std::string hyperlink(std::string_view text, std::string_view url);

/// Text fallback when a terminal cannot render an inline image, exactly as
/// pi's `imageFallback`: `[Image: path [mime] WxH]` with `~/` shortening of
/// home-prefixed absolute paths and an OSC 8 `file://` link when hyperlinks
/// are available and the path is absolute.
[[nodiscard]] std::string image_fallback(
    std::string_view mime_type,
    std::optional<ImagePixelSize> dimensions = std::nullopt,
    std::optional<std::string_view> filename = std::nullopt);

namespace detail {

/// CSI 16 t cell-size query (`ESC [ 16 t`), answered as `ESC [ 6 ; h ; w t`.
inline constexpr std::string_view kCellSizeQuery{"\x1b[16t"};

/// One parsed `ESC [ 6 ; h ; w t` cell-size response.
struct CellSizeResponse {
    std::size_t height_px{0};
    std::size_t width_px{0};

    bool operator==(const CellSizeResponse&) const = default;
};

struct CellSizeInputResult {
    /// Buffered partial response prefix awaiting more input.
    std::string pending{};
    /// Input that is not part of a cell-size response.
    std::string forwarded_input{};
    /// Complete responses extracted from the input stream.
    std::vector<CellSizeResponse> responses{};
};

/// Extract complete `ESC [ 6 ; h ; w t` cell-size responses from `input`,
/// buffering partial leading fragments in `pending` exactly as the keyboard
/// and appearance response parsers do. Fragments shorter than the full
/// `ESC [ 6 ;` prefix are never buffered, so a bare ESC is forwarded
/// immediately (pi's tui-cell-size-input expectation).
[[nodiscard]] CellSizeInputResult consume_cell_size_input(
    std::string pending,
    std::string_view input);

[[nodiscard]] bool cell_regions_intersect(
    const CellRegion& left,
    const CellRegion& right);
[[nodiscard]] bool protocol_supports_mime(
    InlineImageProtocol protocol,
    std::string_view mime_type);

[[nodiscard]] util::Expected<std::string> encode_terminal_image(
    InlineImageProtocol protocol,
    const TerminalImage& image,
    TerminalImageHandle handle);

[[nodiscard]] util::Expected<std::string> encode_terminal_image_removal(
    InlineImageProtocol protocol,
    TerminalImageHandle handle);

} // namespace detail

} // namespace cch::tui

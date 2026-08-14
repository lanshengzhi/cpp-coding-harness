#pragma once

#include <cch/support/Error.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace cch::tui {

struct TerminalDimensions {
    std::size_t columns{80};
    std::size_t rows{24};

    bool operator==(const TerminalDimensions&) const = default;
};

struct CursorPosition {
    std::size_t column{0};
    std::size_t row{0};

    bool operator==(const CursorPosition&) const = default;
};

struct CellRegion {
    std::size_t column{0};
    std::size_t row{0};
    std::size_t columns{0};
    std::size_t rows{0};

    bool operator==(const CellRegion&) const = default;
};

enum class InlineImageProtocol {
    None,
    Kitty,
    ITerm2,
};

enum class KeyboardProtocol {
    Legacy,
    ModifyOtherKeys,
    Kitty,
};

enum class TerminalColorCapability {
    Xterm256,
    TrueColor,
};

enum class TerminalAppearance {
    Unknown,
    Dark,
    Light,
};

struct CellPixelDimensions {
    std::size_t width{9};
    std::size_t height{18};

    bool operator==(const CellPixelDimensions&) const = default;
};

struct TerminalModeState {
    bool started{false};
    bool raw_input{false};
    bool bracketed_paste{false};
    bool cursor_visible{true};

    bool operator==(const TerminalModeState&) const = default;
};

struct TerminalCapabilities {
    bool synchronized_output{false};
    InlineImageProtocol inline_images{InlineImageProtocol::None};
    /// OSC 8 hyperlink support from the per-emulator env rules (pi
    /// terminal-image.ts detectCapabilities).
    bool hyperlinks{false};
    /// Cell size in pixels; pi's 9x18 default applies until a `CSI 16 t`
    /// response (`ESC [ 6 ; h ; w t`) refines it.
    std::optional<CellPixelDimensions> cell_pixels{CellPixelDimensions{}};
    KeyboardProtocol keyboard_protocol{KeyboardProtocol::Legacy};
    TerminalColorCapability color{TerminalColorCapability::Xterm256};
    TerminalAppearance appearance{TerminalAppearance::Unknown};
};

struct TerminalImageHandle {
    std::uint64_t value{0};

    bool operator==(const TerminalImageHandle&) const = default;
};

/// Borrowed image data consumed synchronously by Terminal::place_image().
struct TerminalImage {
    std::string_view encoded_data{};
    std::string_view mime_type{};
    std::optional<std::string_view> filename{std::nullopt};
    std::size_t pixel_width{0};
    std::size_t pixel_height{0};
    std::uint64_t resource_id{0};
    std::uint64_t revision{0};
    CellRegion region{};
    /// When set, re-place with this existing handle instead of allocating a
    /// fresh one (pi's imageId reuse): the Kitty protocol re-transmits with
    /// the same `i=` so animation frames replace in place.
    std::optional<TerminalImageHandle> preferred_handle{std::nullopt};
};

/// Delivers raw input bytes. An empty value flushes an incomplete escape sequence
/// after the terminal's ambiguity timeout.
using TerminalInputSink = std::move_only_function<void(std::string)>;
/// Notifies that the terminal presentation changed and should be re-rendered:
/// terminal dimensions changed, or a presentation-affecting capability update
/// (the `CSI 16 t` cell-size response) was absorbed with unchanged dimensions.
using TerminalResizeSink = std::move_only_function<void(TerminalDimensions)>;

// Behavioral baseline: pi 83114817 packages/tui/src/terminal.ts (ProcessTerminal
// setTitle/setProgress/drainInput and the TERMINAL_PROGRESS_* constants).

/// drain_input() defaults matching pi's drainInput(maxMs = 1000, idleMs = 50).
constexpr auto kDrainInputMaxMs = std::chrono::milliseconds(1000);
constexpr auto kDrainInputIdleMs = std::chrono::milliseconds(50);

class Terminal {
public:
    virtual ~Terminal() = default;

    [[nodiscard]] virtual support::ExpectedVoid start(
        TerminalInputSink input_sink,
        TerminalResizeSink resize_sink) = 0;
    [[nodiscard]] virtual support::ExpectedVoid stop() = 0;
    [[nodiscard]] virtual TerminalDimensions dimensions() const = 0;
    [[nodiscard]] virtual TerminalCapabilities capabilities() const = 0;
    [[nodiscard]] virtual TerminalModeState modes() const = 0;
    /// Clear the terminal's main screen, home the cursor, and clear its
    /// scroll history (`\x1b[2J\x1b[H\x1b[3J`), exactly as pi's resize
    /// full-redraw does (ADR 0037 main-screen scrollback flow).
    [[nodiscard]] virtual support::ExpectedVoid clear_screen() = 0;
    [[nodiscard]] virtual support::ExpectedVoid write(std::string_view output) = 0;
    [[nodiscard]] virtual support::ExpectedVoid set_cursor(CursorPosition position) = 0;
    [[nodiscard]] virtual support::ExpectedVoid set_cursor_visible(bool visible) = 0;

    /// Place one validated image inside a TUI-owned absolute cell region. The
    /// terminal positions the cursor at the region origin before emitting the
    /// protocol sequence; the returned handle becomes the protocol image ID.
    [[nodiscard]] virtual support::Expected<TerminalImageHandle> place_image(const TerminalImage& image) = 0;

    /// Remove one image previously returned by place_image(). The region is supplied
    /// so protocols without addressable deletion can clear exactly the owned cells.
    [[nodiscard]] virtual support::ExpectedVoid remove_image(
        TerminalImageHandle handle,
        const CellRegion& region) = 0;

    /// Begin a synchronized update region. Intermediate writes are not displayed
    /// until end_synchronized_update(). Terminals that do not support this
    /// capability may ignore the start/end markers.
    [[nodiscard]] virtual support::ExpectedVoid begin_synchronized_update() = 0;
    [[nodiscard]] virtual support::ExpectedVoid end_synchronized_update() = 0;

    /// Set the terminal window title (OSC 0;title BEL), exactly as pi's
    /// interactive boot does with setTitle.
    [[nodiscard]] virtual support::ExpectedVoid set_title(std::string_view title) = 0;

    /// Present or clear the progress indicator (OSC 9;4). While active, the
    /// sequence is re-emitted on pi's 1-second keepalive so the indicator
    /// survives terminal redraws and timeouts; stop() clears an active
    /// indicator during mode restoration.
    [[nodiscard]] virtual support::ExpectedVoid set_progress(bool active) = 0;

    /// Drain buffered input before exit so Kitty key-release escape sequences
    /// cannot leak into the parent shell after the TUI stops. Keyboard
    /// protocol push and modifyOtherKeys are disabled first, then input is
    /// consumed (discarded) until idle_ms passes without data or max_ms
    /// elapses, matching pi's drainInput defaults (1000ms / 50ms).
    [[nodiscard]] virtual support::ExpectedVoid drain_input(
        std::chrono::milliseconds max_ms = kDrainInputMaxMs,
        std::chrono::milliseconds idle_ms = kDrainInputIdleMs) = 0;
};

} // namespace cch::tui

#pragma once

#include <cch/util/Error.hpp>

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
    std::optional<CellPixelDimensions> cell_pixels{std::nullopt};
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
};

/// Delivers raw input bytes. An empty value flushes an incomplete escape sequence
/// after the terminal's ambiguity timeout.
using TerminalInputSink = std::move_only_function<void(std::string)>;
using TerminalResizeSink = std::move_only_function<void(TerminalDimensions)>;

// Behavioral baseline: pi 83114817 packages/tui/src/terminal.ts (ProcessTerminal
// setTitle/setProgress/drainInput and the TERMINAL_PROGRESS_* constants).

/// drain_input() defaults matching pi's drainInput(maxMs = 1000, idleMs = 50).
constexpr auto kDrainInputMaxMs = std::chrono::milliseconds(1000);
constexpr auto kDrainInputIdleMs = std::chrono::milliseconds(50);

class Terminal {
public:
    virtual ~Terminal() = default;

    [[nodiscard]] virtual util::ExpectedVoid start(
        TerminalInputSink input_sink,
        TerminalResizeSink resize_sink) = 0;
    [[nodiscard]] virtual util::ExpectedVoid stop() = 0;
    [[nodiscard]] virtual TerminalDimensions dimensions() const = 0;
    [[nodiscard]] virtual TerminalCapabilities capabilities() const = 0;
    [[nodiscard]] virtual TerminalModeState modes() const = 0;
    [[nodiscard]] virtual util::ExpectedVoid clear_screen() = 0;
    [[nodiscard]] virtual util::ExpectedVoid write(std::string_view output) = 0;
    [[nodiscard]] virtual util::ExpectedVoid set_cursor(CursorPosition position) = 0;
    [[nodiscard]] virtual util::ExpectedVoid set_cursor_visible(bool visible) = 0;

    /// Place one validated image inside a TUI-owned absolute cell region.
    [[nodiscard]] virtual util::Expected<TerminalImageHandle> place_image(const TerminalImage& image) = 0;

    /// Remove one image previously returned by place_image(). The region is supplied
    /// so protocols without addressable deletion can clear exactly the owned cells.
    [[nodiscard]] virtual util::ExpectedVoid remove_image(
        TerminalImageHandle handle,
        const CellRegion& region) = 0;

    /// Begin a synchronized update region. Intermediate writes are not displayed
    /// until end_synchronized_update(). Terminals that do not support this
    /// capability may ignore the start/end markers.
    [[nodiscard]] virtual util::ExpectedVoid begin_synchronized_update() = 0;
    [[nodiscard]] virtual util::ExpectedVoid end_synchronized_update() = 0;

    /// Set the terminal window title (OSC 0;title BEL), exactly as pi's
    /// interactive boot does with setTitle.
    [[nodiscard]] virtual util::ExpectedVoid set_title(std::string_view title) = 0;

    /// Present or clear the progress indicator (OSC 9;4). While active, the
    /// sequence is re-emitted on pi's 1-second keepalive so the indicator
    /// survives terminal redraws and timeouts; stop() clears an active
    /// indicator during mode restoration.
    [[nodiscard]] virtual util::ExpectedVoid set_progress(bool active) = 0;

    /// Drain buffered input before exit so Kitty key-release escape sequences
    /// cannot leak into the parent shell after the TUI stops. Keyboard
    /// protocol push and modifyOtherKeys are disabled first, then input is
    /// consumed (discarded) until idle_ms passes without data or max_ms
    /// elapses, matching pi's drainInput defaults (1000ms / 50ms).
    [[nodiscard]] virtual util::ExpectedVoid drain_input(
        std::chrono::milliseconds max_ms = kDrainInputMaxMs,
        std::chrono::milliseconds idle_ms = kDrainInputIdleMs) = 0;
};

} // namespace cch::tui

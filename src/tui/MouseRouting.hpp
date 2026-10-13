#pragma once

#include <cch/tui/Mouse.hpp>

#include <cstddef>
#include <optional>
#include <string_view>
#include <vector>

namespace cch::tui::detail {

/// One parsed pointer report (pi `SgrMouseEvent`, `WheelEvent`, and the legacy
/// X10 form `parseWheelEvent` accepts).
struct MousePacket {
    enum class Kind { Pointer, Wheel };

    Kind kind{Kind::Pointer};
    /// Raw button code as sent by the terminal: bits 0-1 select the button,
    /// bit 2 shift, bit 3 alt, bit 4 ctrl, bit 5 motion, bit 6 wheel/trackpad.
    int button{0};
    /// Zero-based terminal cell.
    std::ptrdiff_t x{0};
    std::ptrdiff_t y{0};
    /// Trailing `m` of an SGR report: the button was released.
    bool release{false};
    /// Wheel direction: -1 up, +1 down (pi `WheelEvent.direction`).
    std::ptrdiff_t direction{0};
};

/// Parse one complete mouse report: SGR (`\x1b[<b;x;yM` / `...m`) or the legacy
/// X10 form (`\x1b[M` plus three bytes). A wheel report is recognized first, so
/// its button code never becomes a press (pi `parseWheelEvent` before
/// `parseSgrMouseEvent`, `tui-alt-screen.ts:957-1016`).
[[nodiscard]] std::optional<MousePacket> parse_mouse_packet(std::string_view data);

/// Whether a fragment can only be a mouse report, and therefore must never
/// reach typed input (pi `TuiAltScreen.isMouseSequence`).
[[nodiscard]] bool is_mouse_sequence(std::string_view data) noexcept;

/// pi `decodeMouseButton`.
[[nodiscard]] MouseButton decode_mouse_button(int button) noexcept;

/// pi `handleMouseEvent`: the event type of a pointer report. The motion bit
/// makes a report a `Move` when it carries no button and a `Drag` when it does.
[[nodiscard]] MouseEventType pointer_event_type(const MousePacket& packet) noexcept;

/// The normalized event of one report (pi `TuiAltScreen.createMouseEvent`):
/// `screen_x`/`screen_y` are the terminal coordinates of the report, the local
/// `x`/`y` start equal to them, `width`/`height` start as the terminal size, and
/// the modifier and button bits are decoded. A layout box or a captured target
/// retargets the local coordinates afterwards.
[[nodiscard]] MouseEvent create_mouse_event(const MousePacket& packet,
        MouseEventType type,
        std::size_t terminal_columns,
        std::size_t terminal_rows,
        std::optional<std::ptrdiff_t> wheel_delta = std::nullopt,
        std::optional<std::size_t> click_count = std::nullopt);

/// Offer `event` to the child whose cached composition rows contain it (pi
/// `Container.handleMouse`, `tui.ts:372-396`). The rows come from the last
/// composition, so a hit test never renders; a caller without a usable cache
/// passes a child count of zero. `child_at` returns the child at an index and
/// `row_at` the rows it occupied.
template <typename ChildAt, typename RowAt>
[[nodiscard]] std::optional<MouseDispatchResult> dispatch_mouse_to_cached_rows(
        const MouseEvent& event, std::size_t child_count, ChildAt&& child_at, RowAt&& row_at) {
    if (event.y < 0 || static_cast<std::size_t>(event.y) >= event.height) return std::nullopt;
    std::ptrdiff_t child_y = 0;
    for (std::size_t index = 0; index < child_count; ++index) {
        const auto child_height = row_at(index);
        if (event.y >= child_y && event.y < child_y + static_cast<std::ptrdiff_t>(child_height)) {
            auto child_event = event;
            child_event.y = event.y - child_y;
            child_event.height = child_height;
            return dispatch_mouse_event(child_at(index), child_event);
        }
        child_y += static_cast<std::ptrdiff_t>(child_height);
    }
    return std::nullopt;
}

} // namespace cch::tui::detail

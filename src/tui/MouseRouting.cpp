#include "tui/MouseRouting.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <string_view>

namespace cch::tui::detail {
namespace {

/// Parse one decimal field of an SGR report; `std::nullopt` for a non-digit.
[[nodiscard]] std::optional<int> parse_decimal(std::string_view text) {
    if (text.empty()) return std::nullopt;
    int value = 0;
    const auto* first = text.data();
    const auto* last = first + text.size();
    const auto [end, error] = std::from_chars(first, last, value);
    if (error != std::errc{} || end != last) return std::nullopt;
    return value;
}

/// `\x1b[<b;x;yM` or `\x1b[<b;x;ym` (pi `parseSgrMouseEvent`).
[[nodiscard]] std::optional<MousePacket> parse_sgr_report(std::string_view data) {
    if (!data.starts_with("\x1b[<") || data.size() < 6) return std::nullopt;
    const auto final = data.back();
    if (final != 'M' && final != 'm') return std::nullopt;
    const auto body = data.substr(3, data.size() - 4);
    std::array<std::string_view, 3> fields{};
    std::size_t field = 0;
    std::size_t start = 0;
    while (start <= body.size()) {
        const auto separator = body.find(';', start);
        const auto part = body.substr(start, separator - start);
        if (field >= fields.size()) return std::nullopt;
        fields[field++] = part;
        if (separator == std::string_view::npos) break;
        start = separator + 1;
    }
    if (field != fields.size()) return std::nullopt;
    const auto button = parse_decimal(fields[0]);
    const auto x = parse_decimal(fields[1]);
    const auto y = parse_decimal(fields[2]);
    if (!button || !x || !y) return std::nullopt;
    return MousePacket{.kind = MousePacket::Kind::Pointer,
            .button = *button,
            .x = static_cast<std::ptrdiff_t>(*x) - 1,
            .y = static_cast<std::ptrdiff_t>(*y) - 1,
            .release = final == 'm',
            .direction = 0};
}

/// `\x1b[M` plus three payload bytes (pi's legacy branch).
[[nodiscard]] std::optional<MousePacket> parse_legacy_report(std::string_view data) {
    if (data.size() != 6 || !data.starts_with("\x1b[M")) return std::nullopt;
    const auto byte = [&](std::size_t index) { return static_cast<unsigned char>(data[index]); };
    return MousePacket{.kind = MousePacket::Kind::Pointer,
            .button = static_cast<int>(byte(3)) - 32,
            .x = static_cast<std::ptrdiff_t>(byte(4)) - 33,
            .y = static_cast<std::ptrdiff_t>(byte(5)) - 33,
            .release = false,
            .direction = 0};
}

/// The wheel direction of a report, or nothing when the button code is not a
/// vertical wheel notch (pi `parseWheelEvent`).
[[nodiscard]] std::optional<std::ptrdiff_t> wheel_direction(int button) noexcept {
    if ((button & 64) == 0) return std::nullopt;
    const auto direction = button & 3;
    if (direction != 0 && direction != 1) return std::nullopt;
    return direction == 0 ? std::ptrdiff_t{-1} : std::ptrdiff_t{1};
}

} // namespace

std::optional<MousePacket> parse_mouse_packet(std::string_view data) {
    if (auto packet = parse_sgr_report(data)) {
        if (const auto direction = wheel_direction(packet->button)) {
            packet->kind = MousePacket::Kind::Wheel;
            packet->direction = *direction;
        }
        return packet;
    }
    // The legacy X10 form is recognized for wheel notches only: a legacy pointer
    // report is consumed as a mouse sequence and produces no event, because the
    // pointer reports of this root are SGR (pi `parseWheelEvent`'s
    // `data.length === 6` branch).
    if (auto legacy = parse_legacy_report(data)) {
        if (const auto direction = wheel_direction(legacy->button)) {
            legacy->kind = MousePacket::Kind::Wheel;
            legacy->direction = *direction;
            return legacy;
        }
    }
    return std::nullopt;
}

bool is_mouse_sequence(std::string_view data) noexcept {
    return parse_sgr_report(data).has_value() || (data.size() == 6 && data.starts_with("\x1b[M"));
}

MouseButton decode_mouse_button(int button) noexcept {
    switch (button & 3) {
    case 0:
        return MouseButton::Left;
    case 1:
        return MouseButton::Middle;
    case 2:
        return MouseButton::Right;
    default:
        return MouseButton::None;
    }
}

MouseEventType pointer_event_type(const MousePacket& packet) noexcept {
    if (packet.release) return MouseEventType::Release;
    if ((packet.button & 32) != 0) {
        return decode_mouse_button(packet.button) == MouseButton::None ? MouseEventType::Move : MouseEventType::Drag;
    }
    return MouseEventType::Press;
}

MouseEvent create_mouse_event(const MousePacket& packet,
        MouseEventType type,
        std::size_t terminal_columns,
        std::size_t terminal_rows,
        std::optional<std::ptrdiff_t> wheel_delta,
        std::optional<std::size_t> click_count) {
    return MouseEvent{.type = type,
            .button = type == MouseEventType::Wheel ? MouseButton::None : decode_mouse_button(packet.button),
            .x = packet.x,
            .y = packet.y,
            .screen_x = packet.x,
            .screen_y = packet.y,
            .width = std::max<std::size_t>(1, terminal_columns),
            .height = std::max<std::size_t>(1, terminal_rows),
            .shift = (packet.button & 4) != 0,
            .alt = (packet.button & 8) != 0,
            .ctrl = (packet.button & 16) != 0,
            .wheel_delta = wheel_delta,
            .click_count = click_count};
}

} // namespace cch::tui::detail

#pragma once

#include <cch/tui/Terminal.hpp>
#include <cch/tui/TerminalColors.hpp>
#include <cch/tui/TerminalImage.hpp>

#include <optional>
#include <variant>

// Out-of-band terminal control responses the decoder demuxes from the raw
// input stream and the TUI root consumes before typed input dispatch. The
// types live on the public surface because `MainScreen` handles them
// (color-query completion, live scheme reports, keyboard negotiation), not
// only the decoder that produces them.

namespace cch::tui::detail {

/// Kitty keyboard-protocol negotiation answers (behavioral baseline: pi
/// 83114817 packages/tui/src/terminal.ts
/// parseKeyboardProtocolNegotiationSequence): the `CSI ? flags u` flags
/// report and the `CSI ? ... c` device-attributes sentinel that triggers the
/// modifyOtherKeys fallback.
enum class KeyboardProtocolResponseKind {
    KittyFlags,
    DeviceAttributes,
};

struct KeyboardProtocolResponse {
    KeyboardProtocolResponseKind kind{KeyboardProtocolResponseKind::DeviceAttributes};
    unsigned int flags{0};
};

/// Terminal color answers (behavioral baseline: pi 83114817
/// packages/tui/src/terminal-colors.ts parseTerminalColorSchemeReport and
/// parseOscColorResponse): the `CSI ? 997 ; 1|2 n` color-scheme report and
/// the OSC 10/11/4 color replies.
enum class ColorSchemeResponseKind {
    ColorScheme,
    Foreground,
    Background,
    Palette,
};

struct ColorSchemeResponse {
    ColorSchemeResponseKind kind{ColorSchemeResponseKind::ColorScheme};
    TerminalAppearance appearance{TerminalAppearance::Unknown};
    std::optional<OscColorResponse> color_response{std::nullopt};
};

/// Out-of-band terminal control responses demuxed from the raw input stream:
/// the CPR answer to the startup DSR query (`ESC [ rows ; cols R`, ADR 0041),
/// the `ESC [ 6 ; h ; w t` cell-size answer, keyboard-protocol negotiation,
/// and color-scheme/color reports.
using TerminalResponseVariant =
        std::variant<CursorPosition, CellSizeResponse, KeyboardProtocolResponse, ColorSchemeResponse>;

} // namespace cch::tui::detail

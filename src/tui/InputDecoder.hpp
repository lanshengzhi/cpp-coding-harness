#pragma once

#include <cch/tui/Keys.hpp>
#include <cch/tui/Terminal.hpp>
#include <cch/tui/TerminalImage.hpp>

#include <string>
#include <string_view>
#include <variant>
#include <vector>

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

/// Terminal appearance answers (behavioral baseline: pi 83114817
/// packages/tui/src/terminal-colors.ts parseTerminalColorSchemeReport and
/// parseOsc11BackgroundColor): the `CSI ? 997 ; 1|2 n` color-scheme report
/// and the OSC 11 background-color report.
enum class ColorSchemeResponseKind {
    ColorScheme,
    Background,
};

struct ColorSchemeResponse {
    ColorSchemeResponseKind kind{ColorSchemeResponseKind::ColorScheme};
    TerminalAppearance appearance{TerminalAppearance::Unknown};
};

/// Out-of-band terminal control responses demuxed from the raw input stream:
/// the CPR answer to the startup DSR query (`ESC [ rows ; cols R`, ADR 0041),
/// the `ESC [ 6 ; h ; w t` cell-size answer, keyboard-protocol negotiation,
/// and color-scheme/background appearance reports.
using TerminalResponseVariant = std::variant<
    CursorPosition,
    CellSizeResponse,
    KeyboardProtocolResponse,
    ColorSchemeResponse>;

/// One value-batched decoder result (CODING_STANDARDS.md §3.3, §4 data
/// contracts): `responses` holds the demuxed out-of-band terminal control
/// answers and `events` the decoded in-band key/paste input, while
/// `forwarded_input` carries the same in-band bytes verbatim. Event consumers
/// (Tui) read `events`; byte-level consumers (the terminal delivery worker
/// and the startup probes) forward `forwarded_input` to the input sink. The
/// two views never need combining: every in-band byte appears in
/// `forwarded_input` exactly once, whether or not it decoded to an event.
struct StreamDecodeResult {
    std::vector<TerminalResponseVariant> responses;
    std::vector<InputEventVariant> events;
    std::string forwarded_input;
};

/// Deep stream decoder for the terminal input edge: one fragment buffer
/// reassembles escape sequences split across reads, one pass demuxes raw
/// byte chunks into out-of-band `TerminalResponseVariant` values and in-band
/// `InputEventVariant` values, and one escape-discard machine bounds
/// malformed input. `feed` consumes a raw chunk; `flush` ends the 150 ms
/// fragment window (the caller's deadline), resolving or dropping whatever
/// the buffer holds. Behavioral baseline: pi 83114817 packages/tui/src
/// (keys.ts parseKey sequences, stdin-buffer.ts framing, terminal.ts
/// negotiation buffering, terminal-image.ts consumeCellSizeResponse, and
/// terminal-colors.ts appearance reports).
class TerminalStreamDecoder final {
public:
    [[nodiscard]] StreamDecodeResult feed(std::string_view input);
    [[nodiscard]] StreamDecodeResult flush();
    void reset();

private:
    enum class EscapeDiscardMode {
        None,
        Csi,
        Osc,
        StringTerminated,
    };

    void drain(StreamDecodeResult& result, bool end_of_feed);
    [[nodiscard]] bool discard_escape_byte(char byte);
    void enter_paste(StreamDecodeResult& result);
    void consume_paste_byte(char byte, StreamDecodeResult& result);
    void commit_paste_byte(char byte);

    std::string pending_;
    EscapeDiscardMode discard_mode_{EscapeDiscardMode::None};
    bool discard_saw_escape_{false};
    bool paste_mode_{false};
    std::string paste_text_;
    std::string paste_end_candidate_;
    std::size_t paste_original_bytes_{0};
    std::size_t paste_lines_{1};
};

} // namespace cch::tui::detail

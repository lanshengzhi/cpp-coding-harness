#pragma once

// The fake/hardware cursor marker layer (pi `packages/tui/src/tui.ts` at the
// pi-v1.3.0 baseline): the zero-width APC sequences a cursor-bearing component
// emits, and the two stages that resolve them into the drawn cursor.
//
// A focused component emits `kCursorMarker` at the cursor position and wraps
// the drawn cursor cell in `kFakeCursorStart`/`kFakeCursorEnd`. The markers
// are zero-width APC sequences that a terminal ignores, so the drawn cursor
// stays strippable: removing the markers leaves exactly the visible text.
//
// `resolve_fake_cursors` is the one stage that turns the fake-cursor markers
// into the drawn cursor. It runs before overlay compositing and before
// selection highlighting, because both track SGR codes rather than marker
// bytes. In hardware-cursor mode the focused fake cursor loses its
// reverse-video wrappers, because the terminal cursor is drawn at that
// position; a fake cursor of an unfocused component carries no `kCursorMarker`
// and stays visible.
//
// `extract_cursor_position` runs after the overlay compositing (pi
// `extractCursorPosition`): it measures the cursor column on the text before
// the marker and removes the marker, so the marker never reaches the
// terminal.

#include <cch/tui/Terminal.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cch::tui {

/// The zero-width cursor-position marker (pi `CURSOR_MARKER`). A focused
/// component emits it immediately before the drawn cursor cell.
inline constexpr std::string_view kCursorMarker{"\x1b_pi:c\x07"};

/// The zero-width marker that opens the drawn cursor cell (pi
/// `FAKE_CURSOR_START`).
inline constexpr std::string_view kFakeCursorStart{"\x1b_pi:fc\x07"};

/// The zero-width marker that closes the drawn cursor cell (pi
/// `FAKE_CURSOR_END`).
inline constexpr std::string_view kFakeCursorEnd{"\x1b_pi:/fc\x07"};

/// Wrap `text` in the fake-cursor markers (pi `renderFakeCursor`).
[[nodiscard]] inline std::string render_fake_cursor(std::string_view text) {
    std::string result;
    result.reserve(kFakeCursorStart.size() + text.size() + kFakeCursorEnd.size());
    result.append(kFakeCursorStart);
    result.append(text);
    result.append(kFakeCursorEnd);
    return result;
}

/// Resolve the fake-cursor markers of `lines` into the drawn cursor (pi
/// `resolveFakeCursors`).
///
/// With `show_hardware_cursor`, the focused fake cursor — the one whose cell
/// immediately follows `kCursorMarker` — loses its reverse-video wrappers,
/// because the terminal cursor is drawn at that position. Every other fake
/// cursor resolves to the reverse-video SGR pair. The cursor marker itself is
/// left in place: the extraction stage reads it after the overlay
/// compositing.
[[nodiscard]] std::vector<std::string> resolve_fake_cursors(
        const std::vector<std::string>& lines, bool show_hardware_cursor);

/// Extract the cursor position the marker carries and remove the marker from
/// the lines (pi `extractCursorPosition`). Only the visible viewport is
/// scanned, from its bottom row upwards. The lines are modified in place:
/// the marker never reaches the terminal.
[[nodiscard]] std::optional<CursorPosition> extract_cursor_position(
        std::vector<std::string>& lines, std::size_t viewport_height);

} // namespace cch::tui

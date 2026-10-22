#include <cch/tui/CursorMarkers.hpp>

#include <cch/tui/Utils.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::tui {
namespace {

/// The reverse-video SGR pair the fake-cursor markers resolve to (pi
/// `resolveFakeCursors`).
constexpr std::string_view kReverseVideoOn{"\x1b[7m"};
constexpr std::string_view kReverseVideoOff{"\x1b[27m"};

/// Replace every occurrence of `from` in `text` with `to`.
[[nodiscard]] std::string replace_all(std::string_view text, std::string_view from, std::string_view to) {
    if (text.find(from) == std::string_view::npos) return std::string(text);
    std::string result;
    std::size_t position = 0;
    while (position < text.size()) {
        const auto found = text.find(from, position);
        if (found == std::string_view::npos) {
            result.append(text.substr(position));
            break;
        }
        result.append(text.substr(position, found - position));
        result.append(to);
        position = found + from.size();
    }
    return result;
}

} // namespace

std::vector<std::string> resolve_fake_cursors(const std::vector<std::string>& lines, bool show_hardware_cursor) {
    std::vector<std::string> resolved;
    resolved.reserve(lines.size());
    for (const auto& line : lines) {
        auto working = line;

        // The focused fake cursor is the one whose cell immediately follows the
        // cursor marker. In hardware-cursor mode the terminal cursor is drawn
        // at that position, so the drawn cell loses its reverse-video
        // wrappers; the cell's text itself stays (pi's `FOCUSED_FAKE_CURSOR`
        // replacement keeps the captured cell text). A fake cursor of an
        // unfocused component carries no marker and stays visible.
        if (show_hardware_cursor) {
            const auto marker = working.find(kCursorMarker);
            if (marker != std::string::npos) {
                const auto start = marker + kCursorMarker.size();
                if (working.compare(start, kFakeCursorStart.size(), kFakeCursorStart) == 0) {
                    const auto cell_start = start + kFakeCursorStart.size();
                    const auto end = working.find(kFakeCursorEnd, cell_start);
                    // A truncated line keeps the opening marker only; the
                    // closing marker is then absent and the rest of the line
                    // is the cursor cell.
                    //
                    // Remove the closing marker, then the opening marker; the
                    // cell text between them stays.
                    if (end != std::string::npos) {
                        working.erase(end, kFakeCursorEnd.size());
                    }
                    working.erase(start, kFakeCursorStart.size());
                }
            }
        }

        auto result = replace_all(working, kFakeCursorStart, kReverseVideoOn);
        result = replace_all(result, kFakeCursorEnd, kReverseVideoOff);
        resolved.push_back(std::move(result));
    }
    return resolved;
}

std::optional<CursorPosition> extract_cursor_position(std::vector<std::string>& lines, std::size_t viewport_height) {
    // Only the visible viewport is scanned, from its bottom row upwards (pi
    // `extractCursorPosition`).
    const auto viewport_top = lines.size() > viewport_height ? lines.size() - viewport_height : 0;
    for (std::size_t index = lines.size(); index > viewport_top; --index) {
        auto& line = lines[index - 1];
        const auto marker = line.find(kCursorMarker);
        if (marker == std::string::npos) continue;
        // The column is the visible width of the text before the marker; the
        // marker itself never reaches the terminal.
        const auto column = visible_width(line.substr(0, marker));
        line.erase(marker, kCursorMarker.size());
        return CursorPosition{.column = column, .row = index - 1};
    }
    return std::nullopt;
}

} // namespace cch::tui

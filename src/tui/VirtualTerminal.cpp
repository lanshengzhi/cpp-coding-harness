#include <cch/tui/VirtualTerminal.hpp>

#include <cch/tui/TerminalImage.hpp>
#include "tui/UnicodeWidth.hpp"

#include <cch/support/Error.hpp>
#include <algorithm>
#include <charconv>
#include <exception>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace cch::tui {

struct VirtualTerminal::Impl {
    TerminalDimensions dimensions;
    TerminalCapabilities capabilities;
    TerminalModeState modes;
    TerminalInputSink input_sink;
    TerminalResizeSink resize_sink;
    std::vector<std::string> output;
    std::vector<std::string> screen;
    /// Lines that scrolled out of the visible viewport (oldest first). Under
    /// the main-screen scrollback flow (pi `TuiMainScreen`) these are the
    /// terminal's native scrollback: the conversation history surface.
    std::vector<std::string> scrollback;
    std::vector<std::vector<VirtualTerminalCell>> cells;
    std::vector<std::vector<VirtualTerminalCell>> scrollback_cells;
    /// Number of scrolled-out lines (== scrollback_cells.size()): the total
    /// scrolls since start() anchored the buffer origin. Under the anchored
    /// absolute flow (ADR 0041) the first buffer line visible on screen is
    /// `max(0, viewport_top - scroll_origin)`. The renderer tracks its own
    /// viewport top in buffer coordinates.
    std::size_t viewport_top{0};
    std::vector<VirtualTerminalImage> images;
    detail::AnsiStyleState style;
    CursorPosition cursor;
    /// Screen row where buffer row 0 was first written: the double IS the
    /// terminal, so it knows its cursor — start() anchors the origin at the
    /// (possibly seeded) cursor row instead of querying it (ADR 0041). The
    /// screen row of buffer row b is `scroll_origin + b - viewport_top`.
    std::size_t scroll_origin{0};
    struct SavedScreen {
        std::vector<std::vector<VirtualTerminalCell>> cells;
        std::vector<std::vector<VirtualTerminalCell>> scrollback_cells;
        std::size_t viewport_top{0};
        std::size_t scroll_origin{0};
        CursorPosition cursor;
        std::vector<VirtualTerminalImage> images;
        detail::AnsiStyleState style;
        std::uint64_t next_image_handle{1};
    };
    std::optional<SavedScreen> saved_main_screen;
    std::size_t sync_depth{0};
    std::uint64_t next_image_handle{1};

    bool progress_active{false};
    bool clear_screen_called{false};
    bool clear_scrollback_called{false};
    std::size_t margin_top{0};
    std::size_t margin_bottom{0};
    bool margins_active{false};
};

namespace {

constexpr std::string_view kBeginSync = "\x1b[?2026h";
constexpr std::string_view kEndSync = "\x1b[?2026l";

[[nodiscard]] std::string decode_base64(std::string_view input) {
    static constexpr std::string_view kChars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    int val = 0, valb = -8;
    for (unsigned char c : input) {
        if (c == '=') break;
        auto pos = kChars.find(c);
        if (pos == std::string_view::npos) continue;
        val = (val << 6) + static_cast<int>(pos);
        valb += 6;
        if (valb >= 0) {
            out.push_back(static_cast<char>((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return out;
}
// pi's resize full-redraw clears screen, homes, and clears scrollback
// (packages/tui/src/terminal.ts clearScreen). The double records the emitted
// sequence in output() so the byte-level `\x1b[3J` is pinned through the seam.
constexpr std::string_view kClearScreenSequence = "\x1b[2J\x1b[H\x1b[3J";

// Behavioral baseline: pi 83114817 packages/tui/src/terminal.ts
// (setTitle OSC 0, setProgress OSC 9;4 active/clear sequences) and
// packages/tui/src/terminal-image.ts + tui.ts (the CSI 16 t cell-size query
// emitted at startup when images are supported, and the ESC [ 6 ; h ; w t
// response consumption that pi's consumeCellSizeResponse performs). The
// 1-second progress keepalive is ProcessTerminal's serial-worker behavior;
// the double records the emitted sequences only.
constexpr std::string_view kProgressActiveSequence = "\x1b]9;4;3\x07";
constexpr std::string_view kProgressClearSequence = "\x1b]9;4;0;\x07";

/// Build the visible text of one cell row: the graphemes up to the last
/// occupied column (wide-grapheme continuations skipped, empty cells as
/// spaces), trimming trailing blank columns like `screen()` does.
template <typename T>
void append_occupied_line(std::vector<std::string>& destination, const T& cells) {
    std::size_t occupied = 0;
    for (std::size_t column = 0; column < cells.size(); ++column) {
        if (!cells[column].grapheme.empty() || cells[column].continuation) occupied = column + 1;
    }
    auto& line = destination.emplace_back();
    for (std::size_t column = 0; column < occupied; ++column) {
        const auto& cell = cells[column];
        if (cell.continuation) continue;
        line += cell.grapheme.empty() ? " " : cell.grapheme;
    }
}

template <typename T>
void refresh_screen(T& impl) {
    impl.screen.clear();
    impl.screen.reserve(impl.dimensions.rows);
    for (const auto& cells : impl.cells) {
        append_occupied_line(impl.screen, cells);
    }
    impl.scrollback.clear();
    impl.scrollback.reserve(impl.scrollback_cells.size());
    for (const auto& cells : impl.scrollback_cells) {
        append_occupied_line(impl.scrollback, cells);
    }
}

/// Scroll the visible viewport up by `count` lines: the top rows move into
/// the terminal's scrollback (pi `TuiMainScreen` lets overflow advance into
/// the terminal's native scrollback) and the bottom rows become blank.
template <typename T>
void scroll_up(T& impl, std::size_t count) {
    if (count == 0) return;
    const auto top = impl.margins_active ? impl.margin_top : 0;
    const auto bottom =
            impl.margins_active ? impl.margin_bottom : (impl.dimensions.rows > 0 ? impl.dimensions.rows - 1 : 0);
    if (top >= bottom || bottom >= impl.dimensions.rows) return;
    const auto region_height = bottom - top + 1;

    if (!impl.modes.alternate_screen) {
        for (std::size_t i = 0; i < count && i < region_height; ++i) {
            impl.scrollback_cells.push_back(std::move(impl.cells[top + i]));
        }
    }
    for (std::size_t r = top; r + count <= bottom; ++r) {
        impl.cells[r] = std::move(impl.cells[r + count]);
    }
    for (std::size_t r = (count < region_height ? bottom - count + 1 : top); r <= bottom; ++r) {
        impl.cells[r].assign(impl.dimensions.columns, {});
    }
    impl.viewport_top += count;
    refresh_screen(impl);
}

template <typename T>
void resize_cells(T& impl, TerminalDimensions dimensions) {
    std::vector<std::vector<VirtualTerminalCell>> resized(
        dimensions.rows,
        std::vector<VirtualTerminalCell>(dimensions.columns));
    const auto rows = std::min(dimensions.rows, impl.cells.size());
    for (std::size_t row = 0; row < rows; ++row) {
        for (std::size_t column = 0; column < impl.cells[row].size();) {
            const auto& cell = impl.cells[row][column];
            if (cell.continuation || cell.grapheme.empty()) {
                ++column;
                continue;
            }
            std::size_t span = 1;
            while (column + span < impl.cells[row].size() &&
                   impl.cells[row][column + span].continuation) {
                ++span;
            }
            if (column + span <= dimensions.columns) {
                for (std::size_t offset = 0; offset < span; ++offset) {
                    resized[row][column + offset] = impl.cells[row][column + offset];
                }
            }
            column += span;
        }
    }
    impl.dimensions = dimensions;
    impl.cells = std::move(resized);
    // Image regions are buffer-absolute under the main-screen scrollback
    // flow, so only a width change can invalidate a placement; rows beyond
    // the new viewport height are legitimate scrollback placements. The
    // renderer follows every resize with a clear-screen full redraw that
    // removes all placements anyway.
    std::erase_if(impl.images, [&](const auto& image) {
        return image.region.column + image.region.columns > dimensions.columns;
    });
    if (dimensions.rows == 0) {
        impl.cursor = {};
    } else {
        impl.cursor.row = std::min(impl.cursor.row, dimensions.rows - 1);
        impl.cursor.column = std::min(impl.cursor.column, dimensions.columns);
    }
    refresh_screen(impl);
}

void clear_grapheme(std::vector<VirtualTerminalCell>& row, std::size_t column) {
    if (column >= row.size()) return;
    auto owner = column;
    while (owner > 0 && row[owner].continuation) --owner;
    auto end = owner + 1;
    while (end < row.size() && row[end].continuation) ++end;
    if (row[owner].grapheme.empty() && !row[column].continuation) {
        row[column] = {};
        return;
    }
    for (auto index = owner; index < end; ++index) row[index] = {};
}

/// Apply tokenized output at the current cursor: ANSI tokens update the style
/// state, graphemes paint cells and advance the cursor, and a newline (pi's
/// raw line flow, e.g. TuiMainScreen beforeTerminalStop's "\r\n") returns to
/// column 0 and advances one row, scrolling the viewport when the cursor sits
/// on the bottom row.
template <typename T>
void paint_tokens(T& impl, const std::vector<detail::TerminalToken>& tokens) {
    for (const auto& token : tokens) {
        if (token.kind == detail::TerminalTokenKind::Newline) {
            impl.cursor.column = 0;
            if (impl.margins_active) {
                if (impl.cursor.row == impl.margin_bottom) {
                    scroll_up(impl, 1);
                    impl.cursor.row = impl.margin_bottom;
                } else if (impl.cursor.row + 1 >= impl.dimensions.rows) {
                    impl.cursor.row = impl.dimensions.rows - 1;
                } else {
                    ++impl.cursor.row;
                }
            } else {
                if (impl.cursor.row + 1 >= impl.dimensions.rows) {
                    scroll_up(impl, 1);
                    impl.cursor.row = impl.dimensions.rows - 1;
                } else {
                    ++impl.cursor.row;
                }
            }
            continue;
        }
        if (token.kind == detail::TerminalTokenKind::CarriageReturn) {
            impl.cursor.column = 0;
            continue;
        }
        if (token.kind == detail::TerminalTokenKind::ClearLine) {
            if (token.text.find("2J") != std::string::npos) {
                impl.cells.assign(impl.dimensions.rows, std::vector<VirtualTerminalCell>(impl.dimensions.columns));
                impl.scrollback_cells.clear();
                impl.viewport_top = 0;
                impl.scroll_origin = 0;
                impl.images.clear();
                detail::clear_image_metadata();
                impl.cursor = {};
            } else if (token.text.find("3J") != std::string::npos) {
                // Clear-scrollback only: this model keeps scrollback cells with
                // the screen, so a bare 3J drops the history and keeps the screen.
                impl.scrollback_cells.clear();
            } else if (impl.cursor.row < impl.cells.size()) {
                impl.cells[impl.cursor.row].assign(impl.dimensions.columns,
                        VirtualTerminalCell{.grapheme = " ", .continuation = false, .style = {}});
            }
            continue;
        }
        if (token.kind == detail::TerminalTokenKind::CursorMove) {
            if (token.text.ends_with('H')) {
                std::size_t row = 1;
                std::size_t col = 1;
                const auto sub = std::string_view(token.text).substr(2, token.text.size() - 3);
                const auto semi = sub.find(';');
                if (semi != std::string_view::npos) {
                    (void)std::from_chars(sub.data(), sub.data() + semi, row);
                    (void)std::from_chars(sub.data() + semi + 1, sub.data() + sub.size(), col);
                } else if (!sub.empty()) {
                    (void)std::from_chars(sub.data(), sub.data() + sub.size(), row);
                }
                impl.cursor.row = row > 0 ? std::min(row - 1, impl.dimensions.rows - 1) : 0;
                impl.cursor.column = col > 0 ? std::min(col - 1, impl.dimensions.columns) : 0;
            } else {
                std::size_t count = 1;
                const auto sub = std::string_view(token.text).substr(2, token.text.size() - 3);
                if (!sub.empty()) {
                    (void)std::from_chars(sub.data(), sub.data() + sub.size(), count);
                }
                if (token.text.ends_with('B')) {
                    impl.cursor.row = std::min(impl.cursor.row + count, impl.dimensions.rows - 1);
                } else if (token.text.ends_with('A')) {
                    impl.cursor.row = impl.cursor.row > count ? impl.cursor.row - count : 0;
                } else if (token.text.ends_with('G')) {
                    impl.cursor.column = count > 0 ? std::min(count - 1, impl.dimensions.columns) : 0;
                }
            }
            continue;
        }
        if (token.kind != detail::TerminalTokenKind::Grapheme) {
            if (token.text.starts_with("\x1b_G")) {
                if (token.text.find("a=d") != std::string::npos) {
                    if (token.text.find("d=a") != std::string::npos || token.text.find("d=A") != std::string::npos) {
                        impl.images.clear();
                    } else if (token.text.find("d=I") != std::string::npos ||
                               token.text.find("d=i") != std::string::npos) {
                        const auto header = parse_kitty_image_header(token.text);
                        if (header) {
                            for (const auto id : header->ids) {
                                std::erase_if(impl.images, [&](const auto& img) { return img.handle.value == id; });
                            }
                        }
                    }
                } else if (token.text.find("a=T") != std::string::npos || token.text.find("a=t") != std::string::npos) {
                    const auto header = parse_kitty_image_header(token.text);
                    const auto handle_val = header && !header->ids.empty() ? header->ids.front() : std::uint32_t{0};
                    std::size_t cols = 1;
                    const auto c_pos = token.text.find("c=");
                    if (c_pos != std::string::npos) {
                        (void)std::from_chars(
                                token.text.data() + c_pos + 2, token.text.data() + token.text.size(), cols);
                    }
                    const auto handle = TerminalImageHandle{.value = handle_val};
                    const auto meta = detail::lookup_image_metadata(handle);
                    const auto col = impl.cursor.column;
                    const auto row = impl.cursor.row + impl.viewport_top;
                    const auto rows = header ? header->rows : 1;
                    VirtualTerminalImage vt_img{
                            .handle = handle,
                            .resource_id = meta ? meta->resource_id : 0,
                            .revision = meta ? meta->revision : 0,
                            .region = {.column = col, .row = row, .columns = cols, .rows = rows},
                            .protocol = InlineImageProtocol::Kitty,
                            .mime_type = meta ? meta->mime_type : "image/png",
                            .filename = meta ? meta->filename : std::nullopt,
                            .pixel_width = meta ? meta->pixel_width : 0,
                            .pixel_height = meta ? meta->pixel_height : 0,
                    };
                    auto it = std::find_if(impl.images.begin(), impl.images.end(), [&](const auto& img) {
                        return img.handle == handle;
                    });
                    if (it != impl.images.end()) {
                        *it = std::move(vt_img);
                    } else {
                        impl.images.push_back(std::move(vt_img));
                    }
                }
            } else if (token.text.starts_with("\x1b]1337;File=")) {
                const auto colon_pos = token.text.find(':');
                if (colon_pos != std::string::npos) {
                    const auto params = token.text.substr(12, colon_pos - 12);
                    std::size_t cols = 1;
                    std::size_t rows = 1;
                    std::optional<std::string> filename;
                    std::size_t p_start = 0;
                    while (p_start <= params.size()) {
                        const auto p_end = params.find(';', p_start);
                        const auto param = params.substr(
                                p_start, p_end == std::string_view::npos ? params.size() - p_start : p_end - p_start);
                        if (param.starts_with("width=")) {
                            (void)std::from_chars(param.data() + 6, param.data() + param.size(), cols);
                        } else if (param.starts_with("height=")) {
                            (void)std::from_chars(param.data() + 7, param.data() + param.size(), rows);
                        } else if (param.starts_with("name=")) {
                            filename = decode_base64(param.substr(5));
                        }
                        if (p_end == std::string_view::npos) break;
                        p_start = p_end + 1;
                    }
                    const auto handle = TerminalImageHandle{.value = impl.next_image_handle++};
                    const auto col = impl.cursor.column;
                    const auto row = impl.cursor.row + impl.viewport_top;
                    const auto payload = token.text.substr(colon_pos + 1);
                    std::string mime_type = "image/png";
                    if (payload.starts_with("/9j/")) {
                        mime_type = "image/jpeg";
                    } else if (payload.starts_with("R0lGOD")) {
                        mime_type = "image/gif";
                    } else if (payload.starts_with("UklGR")) {
                        mime_type = "image/webp";
                    } else if (filename) {
                        if (filename->ends_with(".jpeg") || filename->ends_with(".jpg")) {
                            mime_type = "image/jpeg";
                        } else if (filename->ends_with(".gif")) {
                            mime_type = "image/gif";
                        } else if (filename->ends_with(".webp")) {
                            mime_type = "image/webp";
                        }
                    }
                    VirtualTerminalImage vt_img{
                            .handle = handle,
                            .resource_id = 0,
                            .revision = 1,
                            .region = {.column = col, .row = row, .columns = cols, .rows = rows},
                            .protocol = InlineImageProtocol::ITerm2,
                            .mime_type = std::move(mime_type),
                            .filename = filename,
                            .pixel_width = 0,
                            .pixel_height = 0,
                    };
                    impl.images.push_back(std::move(vt_img));
                }
            }
            impl.style.process_ansi(token.text);
            continue;
        }
        if (token.width > 0 && impl.cursor.column >= impl.dimensions.columns && impl.modes.autowrap) {
            // Deferred wrap (DECAWM): the cursor parked one past the last
            // column wraps to the next row before this grapheme paints,
            // scrolling when it sits on the bottom row.
            impl.cursor.column = 0;
            if (impl.margins_active) {
                if (impl.cursor.row == impl.margin_bottom) {
                    scroll_up(impl, 1);
                    impl.cursor.row = impl.margin_bottom;
                } else if (impl.cursor.row + 1 >= impl.dimensions.rows) {
                    impl.cursor.row = impl.dimensions.rows - 1;
                } else {
                    ++impl.cursor.row;
                }
            } else {
                if (impl.cursor.row + 1 >= impl.dimensions.rows) {
                    scroll_up(impl, 1);
                    impl.cursor.row = impl.dimensions.rows - 1;
                } else {
                    ++impl.cursor.row;
                }
            }
        }
        if (impl.cursor.row < impl.cells.size()) {
            auto& row = impl.cells[impl.cursor.row];
            if (!impl.modes.autowrap && impl.cursor.column >= impl.dimensions.columns) {
                impl.cursor.column = impl.dimensions.columns - 1;
            }
            if (token.width == 0) {
                if (impl.cursor.column > 0) {
                    auto owner = impl.cursor.column - 1;
                    while (owner > 0 && row[owner].continuation) --owner;
                    row[owner].grapheme += token.text;
                }
                continue;
            }
            for (std::size_t offset = 0; offset < token.width; ++offset) {
                clear_grapheme(row, impl.cursor.column + offset);
            }
            const TerminalStyle style = impl.style;
            row[impl.cursor.column] = {
                .grapheme = token.text,
                .continuation = false,
                .style = style,
            };
            for (std::size_t offset = 1; offset < token.width; ++offset) {
                row[impl.cursor.column + offset] = {
                    .grapheme = {},
                    .continuation = true,
                    .style = style,
                };
            }
        }
        impl.cursor.column += token.width;
        if (!impl.modes.autowrap) impl.cursor.column = std::min(impl.cursor.column, impl.dimensions.columns);
    }
}

template <typename T>
support::ExpectedVoid require_started(const T& impl) {
    if (impl.modes.started) return {};
    return std::unexpected(support::make_error(
        support::ErrorCode::Validation,
        "Virtual Terminal must be started before terminal operations"));
}

} // namespace

VirtualTerminal::VirtualTerminal(VirtualTerminalOptions options)
    : impl_(std::make_unique<Impl>()) {
    impl_->capabilities = options.capabilities;
    resize_cells(*impl_, {.columns = options.columns, .rows = options.rows});
}

VirtualTerminal::VirtualTerminal(VirtualTerminal&&) noexcept = default;
VirtualTerminal& VirtualTerminal::operator=(VirtualTerminal&&) noexcept = default;
VirtualTerminal::~VirtualTerminal() = default;

support::ExpectedVoid VirtualTerminal::start(TerminalInputSink input_sink, TerminalResizeSink resize_sink) {
    if (impl_->modes.started) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "Virtual Terminal is already started"));
    }
    if (impl_->dimensions.columns == 0 || impl_->dimensions.rows == 0) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "Virtual Terminal requires positive dimensions"));
    }
    impl_->input_sink = std::move(input_sink);
    impl_->resize_sink = std::move(resize_sink);
    // Anchor the buffer origin at the current cursor row (ADR 0041): the
    // double knows its cursor, so there is no probe; the default cursor{} keeps
    // the origin at row 0 and every unseeded scenario byte-identical.
    impl_->scroll_origin = impl_->cursor.row;
    impl_->modes = {
        .started = true,
        .raw_input = true,
        .bracketed_paste = true,
        .cursor_visible = true,
    };
    // Protocol-aware recording of pi's startup cell-size query (tui.ts
    // queryCellSize): emitted only when inline images are supported.
    if (impl_->capabilities.inline_images != InlineImageProtocol::None) {
        impl_->output.push_back(std::string(kCellSizeQuery));
    }
    return {};
}

support::ExpectedVoid VirtualTerminal::stop() {
    if (impl_->progress_active) {
        impl_->output.push_back(std::string(kProgressClearSequence));
        impl_->progress_active = false;
    }
    impl_->input_sink = {};
    impl_->resize_sink = {};
    impl_->modes.started = false;
    impl_->modes.raw_input = false;
    impl_->modes.bracketed_paste = false;
    impl_->modes.cursor_visible = true;
    impl_->images.clear();
    if (impl_->margins_active) {
        impl_->output.push_back("\x1b[r");
        impl_->margins_active = false;
        impl_->margin_top = 0;
        impl_->margin_bottom = 0;
    }
    return {};
}

TerminalDimensions VirtualTerminal::dimensions() const {
    return impl_->dimensions;
}

TerminalCapabilities VirtualTerminal::capabilities() const {
    return impl_->capabilities;
}

TerminalModeState VirtualTerminal::modes() const {
    return impl_->modes;
}

support::ExpectedVoid VirtualTerminal::clear_screen() {
    if (auto result = require_started(*impl_); !result) return std::unexpected(result.error());
    impl_->clear_screen_called = true;
    // pi's resize full-redraw emits \x1b[2J\x1b[H\x1b[3J: clear screen, home,
    // clear scrollback. The terminal's scroll history is cleared together
    // with the screen so reflow starts clean.
    impl_->clear_scrollback_called = true;
    impl_->output.push_back(std::string(kClearScreenSequence));
    impl_->cells.assign(
        impl_->dimensions.rows,
        std::vector<VirtualTerminalCell>(impl_->dimensions.columns));
    impl_->scrollback_cells.clear();
    impl_->viewport_top = 0;
    impl_->scroll_origin = 0;
    impl_->images.clear();
    detail::clear_image_metadata();
    impl_->cursor = {};
    refresh_screen(*impl_);
    return {};
}

support::ExpectedVoid VirtualTerminal::begin_synchronized_update() {
    if (auto result = require_started(*impl_); !result) return std::unexpected(result.error());
    if (impl_->sync_depth == 0) {
        impl_->output.push_back(std::string(kBeginSync));
    }
    ++impl_->sync_depth;
    return {};
}

support::ExpectedVoid VirtualTerminal::end_synchronized_update() {
    if (auto result = require_started(*impl_); !result) return std::unexpected(result.error());
    if (impl_->sync_depth == 0) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "Virtual Terminal synchronized update is not active"));
    }
    --impl_->sync_depth;
    if (impl_->sync_depth == 0) {
        impl_->output.push_back(std::string(kEndSync));
    }
    return {};
}

support::ExpectedVoid VirtualTerminal::set_title(std::string_view title) {
    if (auto result = require_started(*impl_); !result) return std::unexpected(result.error());
    impl_->output.push_back(std::format("\x1b]0;{}\x07", title));
    return {};
}

support::ExpectedVoid VirtualTerminal::set_progress(bool active) {
    if (auto result = require_started(*impl_); !result) return std::unexpected(result.error());
    impl_->output.push_back(std::string(active ? kProgressActiveSequence : kProgressClearSequence));
    impl_->progress_active = active;
    return {};
}

support::ExpectedVoid VirtualTerminal::drain_input(
    std::chrono::milliseconds,
    std::chrono::milliseconds) {
    if (auto result = require_started(*impl_); !result) return std::unexpected(result.error());
    // The double delivers input synchronously through inject_input; there is
    // no buffered input to drain. The call is recorded by callers through
    // their own ordering, and ProcessTerminal pins the real drain behavior.
    return {};
}

support::ExpectedVoid VirtualTerminal::write(std::string_view output) {
    if (auto result = require_started(*impl_); !result) return std::unexpected(result.error());
    auto tokens = detail::tokenize_terminal_output(output);
    if (!tokens) return std::unexpected(tokens.error());

    // Each physical line must fit its remaining row width; a newline (pi's raw
    // "\r\n" line flow, e.g. TuiMainScreen beforeTerminalStop) starts a fresh
    // full-width line below. A grapheme arriving with the cursor parked one
    // past the last column wraps to the next row first (the deferred-wrap
    // state a real terminal enters after writing a full-width line).
    std::size_t line_width = 0;
    auto available = impl_->dimensions.columns - impl_->cursor.column;
    for (const auto& token : *tokens) {
        if (token.kind == detail::TerminalTokenKind::Newline) {
            line_width = 0;
            available = impl_->dimensions.columns;
            continue;
        }
        if (token.kind == detail::TerminalTokenKind::CarriageReturn) {
            line_width = 0;
            available = impl_->dimensions.columns;
            continue;
        }
        if (token.kind == detail::TerminalTokenKind::CursorMove || token.kind == detail::TerminalTokenKind::ClearLine) {
            line_width = 0;
            available = impl_->dimensions.columns;
            continue;
        }
        if (token.width > 0 && available == 0) {
            available = impl_->dimensions.columns;
        }
        line_width += token.width;
        if (line_width > available) {
            return std::unexpected(support::make_error(
                support::ErrorCode::Validation,
                "TUI component rendered a line wider than its width bound",
                std::format(
                    "line width {} exceeds visible width {}",
                    line_width,
                    available)));
        }
    }

    std::string normalized;
    for (const auto& token : *tokens) normalized += token.text;
    impl_->output.push_back(std::move(normalized));

    paint_tokens(*impl_, *tokens);
    refresh_screen(*impl_);
    return {};
}

support::ExpectedVoid VirtualTerminal::set_cursor(CursorPosition position) {
    if (auto result = require_started(*impl_); !result) return std::unexpected(result.error());
    if (position.column > impl_->dimensions.columns) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "Virtual Terminal cursor position is outside its dimensions"));
    }
    // `row` is a buffer row under the main-screen scrollback flow: rows at or
    // past the visible viewport bottom advance the terminal's scrollback so
    // the addressed line becomes the bottom visible line (pi writes the full
    // buffer with line flow; the absolute-cursor seam scrolls instead). Under
    // the anchored absolute flow (ADR 0041) the screen row of buffer row b is
    // `scroll_origin + b - viewport_top`; buffer rows above the first visible
    // row are in scrollback and clamp to screen row 0.
    const auto first_visible_row = impl_->viewport_top > impl_->scroll_origin
        ? impl_->viewport_top - impl_->scroll_origin
        : std::size_t{0};
    if (position.row < first_visible_row) {
        impl_->cursor.row = impl_->margins_active ? impl_->margin_top : 0;
    } else {
        const auto screen_row = impl_->scroll_origin + position.row - impl_->viewport_top;
        if (impl_->margins_active) {
            const auto viewport_height = impl_->margin_bottom - impl_->margin_top + 1;
            if (screen_row >= viewport_height) {
                const auto scroll = screen_row - (viewport_height - 1);
                scroll_up(*impl_, scroll);
                impl_->cursor.row = impl_->margin_bottom;
            } else {
                impl_->cursor.row = impl_->margin_top + screen_row;
            }
        } else {
            if (screen_row >= impl_->dimensions.rows) {
                const auto scroll = screen_row - (impl_->dimensions.rows - 1);
                scroll_up(*impl_, scroll);
                impl_->cursor.row = impl_->dimensions.rows - 1;
            } else {
                impl_->cursor.row = screen_row;
            }
        }
    }
    impl_->cursor.column = position.column;
    return {};
}

support::ExpectedVoid VirtualTerminal::set_scroll_margins(std::size_t top_row, std::size_t bottom_row) {
    if (auto result = require_started(*impl_); !result) return std::unexpected(result.error());
    // An equal top and bottom is a valid one-row scroll region (#611).
    if (bottom_row >= impl_->dimensions.rows || top_row > bottom_row) {
        return std::unexpected(
                support::make_error(support::ErrorCode::Validation, "Virtual Terminal scroll margins are invalid"));
    }
    impl_->margin_top = top_row;
    impl_->margin_bottom = bottom_row;
    impl_->margins_active = true;
    impl_->output.push_back(std::format("\x1b[{};{}r", top_row + 1, bottom_row + 1));
    return {};
}

support::ExpectedVoid VirtualTerminal::reset_scroll_margins() {
    if (auto result = require_started(*impl_); !result) return std::unexpected(result.error());
    if (!impl_->margins_active) return {};
    impl_->margins_active = false;
    impl_->margin_top = 0;
    impl_->margin_bottom = 0;
    impl_->output.push_back(std::format("\x1b[1;{}r", impl_->dimensions.rows));
    return {};
}

support::ExpectedVoid VirtualTerminal::set_cursor_visible(bool visible) {
    if (auto result = require_started(*impl_); !result) return std::unexpected(result.error());
    impl_->modes.cursor_visible = visible;
    return {};
}

support::ExpectedVoid VirtualTerminal::set_alternate_screen(bool enabled) {
    if (auto result = require_started(*impl_); !result) return std::unexpected(result.error());
    if (impl_->modes.alternate_screen == enabled) return {};
    if (enabled) {
        // DECSET 1049: save the main buffer and cursor, then show a cleared
        // alternate buffer with no scrollback. The model keeps the saved
        // buffer beside the active cells so leaving the mode restores it.
        Impl::SavedScreen saved;
        saved.cells = std::move(impl_->cells);
        saved.scrollback_cells = std::move(impl_->scrollback_cells);
        saved.viewport_top = impl_->viewport_top;
        saved.scroll_origin = impl_->scroll_origin;
        saved.cursor = impl_->cursor;
        saved.images = std::move(impl_->images);
        saved.style = impl_->style;
        saved.next_image_handle = impl_->next_image_handle;
        impl_->saved_main_screen = std::move(saved);
        impl_->cells.assign(impl_->dimensions.rows, std::vector<VirtualTerminalCell>(impl_->dimensions.columns));
        impl_->scrollback_cells.clear();
        impl_->viewport_top = 0;
        impl_->scroll_origin = 0;
        impl_->cursor = {};
        impl_->images.clear();
        impl_->style = {};
        impl_->modes.alternate_screen = true;
        impl_->output.push_back("\x1b[?1049h");
        refresh_screen(*impl_);
        return {};
    }

    if (impl_->saved_main_screen) {
        auto saved = std::move(*impl_->saved_main_screen);
        impl_->saved_main_screen.reset();
        impl_->cells = std::move(saved.cells);
        impl_->scrollback_cells = std::move(saved.scrollback_cells);
        impl_->viewport_top = saved.viewport_top;
        impl_->scroll_origin = saved.scroll_origin;
        impl_->cursor = saved.cursor;
        impl_->images = std::move(saved.images);
        impl_->style = saved.style;
        impl_->next_image_handle = saved.next_image_handle;
        // A resize while the alternate buffer was active must not leak into
        // the restored main buffer: normalize the saved cells to the current
        // dimensions.
        resize_cells(*impl_, impl_->dimensions);
    }
    impl_->modes.alternate_screen = false;
    impl_->output.push_back("\x1b[?1049l");
    refresh_screen(*impl_);
    return {};
}

support::ExpectedVoid VirtualTerminal::set_autowrap(bool enabled) {
    if (auto result = require_started(*impl_); !result) return std::unexpected(result.error());
    if (impl_->modes.autowrap == enabled) return {};
    impl_->modes.autowrap = enabled;
    impl_->output.push_back(enabled ? "\x1b[?7h" : "\x1b[?7l");
    return {};
}

support::ExpectedVoid VirtualTerminal::seed_shell_content(
    std::vector<std::string> lines,
    CursorPosition cursor) {
    if (impl_->modes.started) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "Virtual Terminal shell content must be seeded before start"));
    }
    if (lines.size() > impl_->dimensions.rows ||
        cursor.row >= impl_->dimensions.rows ||
        cursor.column > impl_->dimensions.columns) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "Virtual Terminal seeded shell content is outside its dimensions"));
    }
    // Test scaffolding for the dirty-screen boot scenario (issue #476, ADR
    // 0041): the shell's pre-existing output occupies the top rows and the
    // cursor sits where the shell left it, so start() anchors the buffer
    // origin there. Seeded content is not program output: it is painted into
    // the cells without touching output() or the image placements.
    for (std::size_t row = 0; row < lines.size(); ++row) {
        auto tokens = detail::tokenize_terminal_output(lines[row]);
        if (!tokens) return std::unexpected(tokens.error());
        std::size_t width = 0;
        for (const auto& token : *tokens) {
            if (token.kind == detail::TerminalTokenKind::Newline) {
                return std::unexpected(support::make_error(
                    support::ErrorCode::Validation,
                    "Virtual Terminal seeded shell content must be single physical lines"));
            }
            width += token.width;
        }
        if (width > impl_->dimensions.columns) {
            return std::unexpected(support::make_error(
                support::ErrorCode::Validation,
                "Virtual Terminal seeded shell line is wider than its dimensions"));
        }
        impl_->cursor = {.column = 0, .row = row};
        paint_tokens(*impl_, *tokens);
    }
    impl_->cursor = cursor;
    refresh_screen(*impl_);
    return {};
}

support::ExpectedVoid VirtualTerminal::inject_input(std::string input) {
    if (!impl_->modes.started || !impl_->input_sink) return {};
    // Delivered verbatim, including a `CSI 6 ; h ; w t` cell-size reply (#952):
    // the host's raw-input listener stage observes the reply before the
    // downstream consumer (apply_cell_pixel_dimensions) refines the cell size.
    // Split fragments reassemble in the host's own decoder.
    if (auto delivered = impl_->input_sink(std::move(input)); !delivered) {
        impl_->input_sink = {};
        return std::unexpected(std::move(delivered.error()));
    }
    return {};
}

support::ExpectedVoid VirtualTerminal::apply_cell_pixel_dimensions(CellPixelDimensions pixels) {
    // pi `consumeCellSizeResponse` (tui.ts): the reply is consumed (never
    // typed) whether or not the size applies. A zero height or width applies
    // nothing and invalidates nothing — but it is still consumed. So `{}`
    // changes no state and wakes no re-render, exactly like pi's
    // `heightPx <= 0 || widthPx <= 0` early-true return.
    if (pixels.width == 0 || pixels.height == 0) return {};
    if (impl_->capabilities.cell_pixels && *impl_->capabilities.cell_pixels == pixels) return {};
    impl_->capabilities.cell_pixels = pixels;
    if (!impl_->modes.started || !impl_->resize_sink) return {};
    if (auto delivered = impl_->resize_sink(impl_->dimensions); !delivered) {
        impl_->resize_sink = {};
        return std::unexpected(std::move(delivered.error()));
    }
    return {};
}

support::ExpectedVoid VirtualTerminal::flush_input() {
    return inject_input({});
}

support::ExpectedVoid VirtualTerminal::inject_resize(TerminalDimensions dimensions) {
    resize_cells(*impl_, dimensions);
    if (!impl_->modes.started || !impl_->resize_sink) return {};
    if (auto delivered = impl_->resize_sink(dimensions); !delivered) {
        impl_->resize_sink = {};
        return std::unexpected(std::move(delivered.error()));
    }
    return {};
}

const std::vector<std::string>& VirtualTerminal::output() const {
    return impl_->output;
}

const std::vector<std::string>& VirtualTerminal::screen() const {
    return impl_->screen;
}

const std::vector<std::string>& VirtualTerminal::scrollback() const {
    return impl_->scrollback;
}

std::size_t VirtualTerminal::viewport_top() const {
    return impl_->viewport_top;
}

const std::vector<std::vector<VirtualTerminalCell>>& VirtualTerminal::cells() const {
    return impl_->cells;
}

const std::vector<std::vector<VirtualTerminalCell>>& VirtualTerminal::scrollback_cells() const {
    return impl_->scrollback_cells;
}

const std::vector<VirtualTerminalImage>& VirtualTerminal::images() const {
    return impl_->images;
}

TerminalStyle VirtualTerminal::final_style() const { return impl_->style; }

CursorPosition VirtualTerminal::cursor() const {
    return impl_->cursor;
}

bool VirtualTerminal::check_clear_screen_called() {
    const auto result = impl_->clear_screen_called;
    impl_->clear_screen_called = false;
    return result;
}

bool VirtualTerminal::check_clear_scrollback_called() {
    const auto result = impl_->clear_scrollback_called;
    impl_->clear_scrollback_called = false;
    return result;
}

} // namespace cch::tui

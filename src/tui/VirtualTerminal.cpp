#include <cch/tui/VirtualTerminal.hpp>

#include <cch/tui/TerminalImage.hpp>
#include "tui/UnicodeWidth.hpp"

#include <cch/util/Error.hpp>
#include <algorithm>
#include <exception>
#include <format>
#include <memory>
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
    /// Number of scrolled-out buffer lines (== scrollback_cells.size()): the
    /// first buffer line visible on screen. The renderer tracks the same
    /// quantity as its viewport top.
    std::size_t viewport_top{0};
    std::vector<VirtualTerminalImage> images;
    detail::AnsiStyleState style;
    CursorPosition cursor;
    std::size_t sync_depth{0};
    std::uint64_t next_image_handle{1};
    std::string cell_size_pending;
    bool progress_active{false};
    bool clear_screen_called{false};
    bool clear_scrollback_called{false};
};

namespace {

constexpr std::string_view kBeginSync = "\x1b[?2026h";
constexpr std::string_view kEndSync = "\x1b[?2026l";
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

[[nodiscard]] VirtualTerminalStyle public_style(const detail::AnsiStyleState& style) {
    return {
        .bold = style.bold,
        .dim = style.dim,
        .italic = style.italic,
        .underline = style.underline,
        .blink = style.blink,
        .inverse = style.inverse,
        .hidden = style.hidden,
        .strikethrough = style.strikethrough,
        .fg_color = style.fg_color,
        .bg_color = style.bg_color,
        .hyperlink = style.hyperlink,
        .hyperlink_params = style.hyperlink_params,
    };
}

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
    const auto keep = impl.dimensions.rows > count ? impl.dimensions.rows - count : 0;
    for (std::size_t row = 0; row < count && row < impl.dimensions.rows; ++row) {
        impl.scrollback_cells.push_back(std::move(impl.cells[row]));
    }
    for (std::size_t row = 0; row < keep; ++row) {
        impl.cells[row] = std::move(impl.cells[row + count]);
    }
    impl.cells.resize(impl.dimensions.rows);
    for (std::size_t row = keep; row < impl.dimensions.rows; ++row) {
        impl.cells[row].assign(impl.dimensions.columns, {});
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

template <typename T>
util::ExpectedVoid require_started(const T& impl) {
    if (impl.modes.started) return {};
    return std::unexpected(util::make_error(
        util::ErrorCode::Validation,
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

util::ExpectedVoid VirtualTerminal::start(TerminalInputSink input_sink, TerminalResizeSink resize_sink) {
    if (impl_->modes.started) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "Virtual Terminal is already started"));
    }
    if (impl_->dimensions.columns == 0 || impl_->dimensions.rows == 0) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "Virtual Terminal requires positive dimensions"));
    }
    impl_->input_sink = std::move(input_sink);
    impl_->resize_sink = std::move(resize_sink);
    impl_->modes = {
        .started = true,
        .raw_input = true,
        .bracketed_paste = true,
        .cursor_visible = true,
    };
    // Protocol-aware recording of pi's startup cell-size query (tui.ts
    // queryCellSize): emitted only when inline images are supported.
    if (impl_->capabilities.inline_images != InlineImageProtocol::None) {
        impl_->output.push_back(std::string(detail::kCellSizeQuery));
    }
    return {};
}

util::ExpectedVoid VirtualTerminal::stop() {
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

util::ExpectedVoid VirtualTerminal::clear_screen() {
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
    impl_->images.clear();
    impl_->cursor = {};
    refresh_screen(*impl_);
    return {};
}

util::ExpectedVoid VirtualTerminal::begin_synchronized_update() {
    if (auto result = require_started(*impl_); !result) return std::unexpected(result.error());
    if (impl_->sync_depth == 0) {
        impl_->output.push_back(std::string(kBeginSync));
    }
    ++impl_->sync_depth;
    return {};
}

util::ExpectedVoid VirtualTerminal::end_synchronized_update() {
    if (auto result = require_started(*impl_); !result) return std::unexpected(result.error());
    if (impl_->sync_depth == 0) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "Virtual Terminal synchronized update is not active"));
    }
    --impl_->sync_depth;
    if (impl_->sync_depth == 0) {
        impl_->output.push_back(std::string(kEndSync));
    }
    return {};
}

util::ExpectedVoid VirtualTerminal::set_title(std::string_view title) {
    if (auto result = require_started(*impl_); !result) return std::unexpected(result.error());
    impl_->output.push_back(std::format("\x1b]0;{}\x07", title));
    return {};
}

util::ExpectedVoid VirtualTerminal::set_progress(bool active) {
    if (auto result = require_started(*impl_); !result) return std::unexpected(result.error());
    impl_->output.push_back(std::string(active ? kProgressActiveSequence : kProgressClearSequence));
    impl_->progress_active = active;
    return {};
}

util::ExpectedVoid VirtualTerminal::drain_input(
    std::chrono::milliseconds,
    std::chrono::milliseconds) {
    if (auto result = require_started(*impl_); !result) return std::unexpected(result.error());
    // The double delivers input synchronously through inject_input; there is
    // no buffered input to drain. The call is recorded by callers through
    // their own ordering, and ProcessTerminal pins the real drain behavior.
    return {};
}

util::ExpectedVoid VirtualTerminal::write(std::string_view output) {
    if (auto result = require_started(*impl_); !result) return std::unexpected(result.error());
    auto tokens = detail::tokenize_terminal_output(output);
    if (!tokens) return std::unexpected(tokens.error());

    std::size_t output_width = 0;
    for (const auto& token : *tokens) {
        if (token.kind == detail::TerminalTokenKind::Newline) {
            return std::unexpected(util::make_error(
                util::ErrorCode::Validation,
                "Virtual Terminal write must contain exactly one physical line"));
        }
        output_width += token.width;
    }
    const auto remaining = impl_->dimensions.columns - impl_->cursor.column;
    if (output_width > remaining) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "TUI component rendered a line wider than its width bound",
            std::format(
                "line width {} exceeds visible width {}",
                output_width,
                remaining)));
    }

    if (output_width > 0) {
        const CellRegion written{
            .column = impl_->cursor.column,
            // The cursor row is screen-relative; image regions are
            // buffer-absolute under the main-screen scrollback flow.
            .row = impl_->cursor.row + impl_->viewport_top,
            .columns = output_width,
            .rows = 1,
        };
        std::erase_if(impl_->images, [&](const auto& image) {
            return detail::cell_regions_intersect(image.region, written);
        });
    }

    std::string normalized;
    for (const auto& token : *tokens) normalized += token.text;
    impl_->output.push_back(std::move(normalized));

    for (const auto& token : *tokens) {
        if (token.kind != detail::TerminalTokenKind::Grapheme) {
            impl_->style.process_ansi(token.text);
            continue;
        }
        if (impl_->cursor.row < impl_->cells.size()) {
            auto& row = impl_->cells[impl_->cursor.row];
            if (token.width == 0) {
                if (impl_->cursor.column > 0) {
                    auto owner = impl_->cursor.column - 1;
                    while (owner > 0 && row[owner].continuation) --owner;
                    row[owner].grapheme += token.text;
                }
                continue;
            }
            for (std::size_t offset = 0; offset < token.width; ++offset) {
                clear_grapheme(row, impl_->cursor.column + offset);
            }
            const auto style = public_style(impl_->style);
            row[impl_->cursor.column] = {
                .grapheme = token.text,
                .continuation = false,
                .style = style,
            };
            for (std::size_t offset = 1; offset < token.width; ++offset) {
                row[impl_->cursor.column + offset] = {
                    .grapheme = {},
                    .continuation = true,
                    .style = style,
                };
            }
        }
        impl_->cursor.column += token.width;
    }
    refresh_screen(*impl_);
    return {};
}

util::ExpectedVoid VirtualTerminal::set_cursor(CursorPosition position) {
    if (auto result = require_started(*impl_); !result) return std::unexpected(result.error());
    if (position.column > impl_->dimensions.columns) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "Virtual Terminal cursor position is outside its dimensions"));
    }
    // `row` is a buffer row under the main-screen scrollback flow: rows at or
    // past the visible viewport bottom advance the terminal's scrollback so
    // the addressed line becomes the bottom visible line (pi writes the full
    // buffer with line flow; the absolute-cursor seam scrolls instead).
    if (position.row >= impl_->viewport_top + impl_->dimensions.rows) {
        const auto scroll = position.row - (impl_->viewport_top + impl_->dimensions.rows - 1);
        scroll_up(*impl_, scroll);
        impl_->cursor.row = impl_->dimensions.rows - 1;
    } else if (position.row < impl_->viewport_top) {
        impl_->cursor.row = 0;
    } else {
        impl_->cursor.row = position.row - impl_->viewport_top;
    }
    impl_->cursor.column = position.column;
    return {};
}

util::ExpectedVoid VirtualTerminal::set_cursor_visible(bool visible) {
    if (auto result = require_started(*impl_); !result) return std::unexpected(result.error());
    impl_->modes.cursor_visible = visible;
    return {};
}

util::Expected<TerminalImageHandle> VirtualTerminal::place_image(const TerminalImage& image) {
    if (auto result = require_started(*impl_); !result) return std::unexpected(result.error());
    if (impl_->capabilities.inline_images == InlineImageProtocol::None) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "Virtual Terminal does not support inline images"));
    }
    if (!detail::protocol_supports_mime(
            impl_->capabilities.inline_images,
            image.mime_type)) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "Inline image format is unsupported by the Virtual Terminal protocol"));
    }
    if (image.region.columns == 0 || image.region.rows == 0 ||
        image.region.column >= impl_->dimensions.columns ||
        image.region.columns > impl_->dimensions.columns - image.region.column) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "Inline image region is outside Virtual Terminal dimensions"));
    }
    for (const auto& visible : impl_->images) {
        if (detail::cell_regions_intersect(visible.region, image.region)) {
            // A re-placement of the same protocol image (an animation frame
            // update) replaces the prior record in place instead of
            // overlapping it.
            if (image.preferred_handle && visible.handle == *image.preferred_handle) {
                continue;
            }
            return std::unexpected(util::make_error(
                util::ErrorCode::Validation,
                "Inline image regions cannot overlap"));
        }
    }

    const TerminalImageHandle handle = image.preferred_handle
        ? *image.preferred_handle
        : TerminalImageHandle{.value = impl_->next_image_handle++};
    // The re-transmit replaces the previous record carrying this handle.
    std::erase_if(impl_->images, [&](const auto& visible) {
        return visible.handle == handle;
    });
    impl_->images.push_back({
        .handle = handle,
        .resource_id = image.resource_id,
        .revision = image.revision,
        .region = image.region,
        .protocol = impl_->capabilities.inline_images,
        .mime_type = std::string(image.mime_type),
        .filename = image.filename ? std::optional<std::string>(*image.filename) : std::nullopt,
        .pixel_width = image.pixel_width,
        .pixel_height = image.pixel_height,
    });
    return handle;
}

util::ExpectedVoid VirtualTerminal::remove_image(
    TerminalImageHandle handle,
    const CellRegion& region) {
    if (auto result = require_started(*impl_); !result) return std::unexpected(result.error());
    const auto found = std::find_if(impl_->images.begin(), impl_->images.end(), [&](const auto& image) {
        return image.handle == handle;
    });
    if (found == impl_->images.end()) return {};
    if (found->region != region) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "Inline image removal region does not match its owned region"));
    }
    const auto owned_region = found->region;
    impl_->images.erase(found);
    // The region is buffer-absolute: only cells inside the visible viewport
    // can be cleared (rows scrolled into the terminal's scrollback keep their
    // content, matching pi's keep-history semantics).
    const auto viewport_start = impl_->viewport_top;
    const auto viewport_end = impl_->viewport_top + impl_->dimensions.rows;
    const auto start_row = std::max(owned_region.row, viewport_start);
    const auto end_row = std::min(owned_region.row + owned_region.rows, viewport_end);
    if (start_row < end_row) {
        const auto end_column = std::min(
            impl_->dimensions.columns,
            owned_region.column + owned_region.columns);
        for (auto row = start_row; row < end_row; ++row) {
            const auto screen_row = row - viewport_start;
            for (auto column = owned_region.column; column < end_column; ++column) {
                clear_grapheme(impl_->cells[screen_row], column);
            }
        }
    }
    refresh_screen(*impl_);
    return {};
}

util::ExpectedVoid VirtualTerminal::inject_input(std::string input) {
    if (!impl_->modes.started || !impl_->input_sink) return {};
    auto consumed = detail::consume_cell_size_input(
        std::move(impl_->cell_size_pending),
        input);
    impl_->cell_size_pending = std::move(consumed.pending);
    if (!consumed.responses.empty()) {
        const auto& response = consumed.responses.back();
        if (response.height_px > 0 && response.width_px > 0) {
            impl_->capabilities.cell_pixels = CellPixelDimensions{
                .width = response.width_px,
                .height = response.height_px,
            };
            if (impl_->resize_sink) {
                try {
                    impl_->resize_sink(impl_->dimensions);
                } catch (const std::exception&) {
                    return std::unexpected(util::make_error(
                        util::ErrorCode::Unknown,
                        "Virtual Terminal resize sink failed",
                        "the resize callback threw an exception"));
                } catch (...) {
                    return std::unexpected(util::make_error(
                        util::ErrorCode::Unknown,
                        "Virtual Terminal resize sink failed",
                        "the resize callback threw an unknown exception"));
                }
            }
        }
    }
    if (input.empty() || !consumed.forwarded_input.empty()) {
        try {
            impl_->input_sink(std::move(consumed.forwarded_input));
        } catch (const std::exception&) {
            return std::unexpected(util::make_error(
                util::ErrorCode::Unknown,
                "Virtual Terminal input sink failed",
                "the input callback threw an exception"));
        } catch (...) {
            return std::unexpected(util::make_error(
                util::ErrorCode::Unknown,
                "Virtual Terminal input sink failed",
                "the input callback threw an unknown exception"));
        }
    }
    return {};
}

util::ExpectedVoid VirtualTerminal::flush_input() {
    return inject_input({});
}

util::ExpectedVoid VirtualTerminal::inject_resize(TerminalDimensions dimensions) {
    resize_cells(*impl_, dimensions);
    if (!impl_->modes.started || !impl_->resize_sink) return {};
    try {
        impl_->resize_sink(dimensions);
    } catch (const std::exception&) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Unknown,
            "Virtual Terminal resize sink failed",
            "the resize callback threw an exception"));
    } catch (...) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Unknown,
            "Virtual Terminal resize sink failed",
            "the resize callback threw an unknown exception"));
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

const std::vector<VirtualTerminalImage>& VirtualTerminal::images() const {
    return impl_->images;
}

VirtualTerminalStyle VirtualTerminal::final_style() const {
    return public_style(impl_->style);
}

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

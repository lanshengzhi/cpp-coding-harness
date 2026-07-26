#include <cch/tui/VirtualTerminal.hpp>

#include "tui/UnicodeWidth.hpp"

#include <algorithm>
#include <cstddef>
#include <exception>
#include <string>
#include <utility>

namespace cch::tui {

struct VirtualTerminal::Impl {
    TerminalDimensions dimensions;
    TerminalModeState modes;
    TerminalInputSink input_sink;
    TerminalResizeSink resize_sink;
    std::vector<std::string> output;
    std::vector<std::string> screen;
    CursorPosition cursor;
};

namespace {

/// Find the byte offset in a string corresponding to a given visible column position.
[[nodiscard]] std::size_t byte_offset_at_column(std::string_view line, std::size_t column) {
    std::size_t col = 0;
    std::size_t byte_pos = 0;

    while (byte_pos < line.size() && col < column) {
        // Skip ANSI codes
        auto ansi = detail::extract_ansi_code(line, byte_pos);
        if (ansi) {
            byte_pos += ansi->length;
            continue;
        }

        auto [cp, bytes] = detail::decode_utf8(line, byte_pos);
        if (bytes == 0) break;

        auto cw = detail::codepoint_width(cp);
        // If this character would exceed the target column, stop
        if (col + static_cast<std::size_t>(cw) > column) {
            break;
        }

        col += static_cast<std::size_t>(cw);
        byte_pos += bytes;

        // Skip following combining marks (they don't advance column)
        while (byte_pos < line.size()) {
            auto [next_cp, next_bytes] = detail::decode_utf8(line, byte_pos);
            if (next_bytes == 0) break;
            if (detail::codepoint_width(next_cp) > 0) break; // not a combining mark
            byte_pos += next_bytes;
        }
    }

    return byte_pos;
}

/// Get the visible width of a line stored in the screen buffer.
[[nodiscard]] std::size_t screen_line_width(std::string_view line) {
    return static_cast<std::size_t>(detail::visible_width(line));
}

template <typename T>
void resize_screen(T& impl) {
    impl.screen.resize(impl.dimensions.rows);
    for (auto& line : impl.screen) {
        const auto line_vis = screen_line_width(line);
        if (line_vis > impl.dimensions.columns) {
            // Truncate the string to fit within the column count
            line = line.substr(0, byte_offset_at_column(line, impl.dimensions.columns));
        }
    }
    if (impl.dimensions.rows == 0) {
        impl.cursor = {};
        return;
    }
    impl.cursor.row = std::min(impl.cursor.row, impl.dimensions.rows - 1);
    // Truncate cursor column to new width
    impl.cursor.column = std::min(impl.cursor.column, impl.dimensions.columns);
}

template <typename T>
util::ExpectedVoid require_started(const T& impl) {
    if (impl.modes.started) {
        return {};
    }
    return std::unexpected(util::make_error(
        util::ErrorCode::Validation,
        "Virtual Terminal must be started before terminal operations"));
}

} // namespace

VirtualTerminal::VirtualTerminal(VirtualTerminalOptions options)
    : impl_(std::make_unique<Impl>()) {
    impl_->dimensions = {.columns = options.columns, .rows = options.rows};
    resize_screen(*impl_);
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
    return {};
}

util::ExpectedVoid VirtualTerminal::stop() {
    impl_->input_sink = {};
    impl_->resize_sink = {};
    impl_->modes.started = false;
    impl_->modes.raw_input = false;
    impl_->modes.bracketed_paste = false;
    impl_->modes.cursor_visible = true;
    return {};
}

TerminalDimensions VirtualTerminal::dimensions() const {
    return impl_->dimensions;
}

TerminalCapabilities VirtualTerminal::capabilities() const {
    return {.synchronized_output = true};
}

TerminalModeState VirtualTerminal::modes() const {
    return impl_->modes;
}

util::ExpectedVoid VirtualTerminal::clear_screen() {
    if (auto result = require_started(*impl_); !result) {
        return std::unexpected(result.error());
    }
    for (auto& line : impl_->screen) {
        line.clear();
    }
    impl_->cursor = {};
    return {};
}

util::ExpectedVoid VirtualTerminal::write(std::string_view output) {
    if (auto result = require_started(*impl_); !result) {
        return std::unexpected(result.error());
    }

    // Normalize and validate the output
    auto normalized = detail::normalize_terminal_output(output);
    if (!normalized) {
        return std::unexpected(normalized.error());
    }

    const auto output_vis_width = static_cast<std::size_t>(detail::visible_width(*normalized));
    const auto remaining = impl_->dimensions.columns - impl_->cursor.column;

    if (output_vis_width > remaining) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "TUI component rendered a line wider than its width bound",
            "line width " + std::to_string(output_vis_width) +
                " exceeds visible width " + std::to_string(remaining)));
    }

    impl_->output.emplace_back(*normalized);

    if (impl_->cursor.row >= impl_->screen.size()) {
        impl_->cursor.column += output_vis_width;
        return {};
    }

    auto& line = impl_->screen[impl_->cursor.row];

    // Pad line to reach cursor column if needed
    const auto current_vis_width = screen_line_width(line);
    if (impl_->cursor.column > current_vis_width) {
        // Need to pad the line with spaces to reach cursor position
        const auto pad_cols = impl_->cursor.column - current_vis_width;
        line.append(std::string(pad_cols, ' '));
    }

    // Find byte offset at cursor column
    const auto target_byte = byte_offset_at_column(line, impl_->cursor.column);
    if (line.size() < target_byte) {
        line.resize(target_byte, ' ');
    }

    // Replace content
    line.replace(target_byte, normalized->size(), *normalized);

    // Advance cursor by visible width
    impl_->cursor.column += output_vis_width;
    return {};
}

util::ExpectedVoid VirtualTerminal::set_cursor(CursorPosition position) {
    if (auto result = require_started(*impl_); !result) {
        return std::unexpected(result.error());
    }
    if (position.row >= impl_->dimensions.rows || position.column > impl_->dimensions.columns) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "Virtual Terminal cursor position is outside its dimensions"));
    }
    impl_->cursor = position;
    return {};
}

util::ExpectedVoid VirtualTerminal::set_cursor_visible(bool visible) {
    if (auto result = require_started(*impl_); !result) {
        return std::unexpected(result.error());
    }
    impl_->modes.cursor_visible = visible;
    return {};
}

util::ExpectedVoid VirtualTerminal::inject_input(std::string input) {
    if (!impl_->modes.started || !impl_->input_sink) {
        return {};
    }
    try {
        impl_->input_sink(std::move(input));
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
    return {};
}

util::ExpectedVoid VirtualTerminal::inject_resize(TerminalDimensions dimensions) {
    impl_->dimensions = dimensions;
    resize_screen(*impl_);
    if (!impl_->modes.started || !impl_->resize_sink) {
        return {};
    }
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

CursorPosition VirtualTerminal::cursor() const {
    return impl_->cursor;
}

} // namespace cch::tui

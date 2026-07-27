#include <cch/tui/VirtualTerminal.hpp>

#include "tui/UnicodeWidth.hpp"

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
    TerminalModeState modes;
    TerminalInputSink input_sink;
    TerminalResizeSink resize_sink;
    std::vector<std::string> output;
    std::vector<std::string> screen;
    std::vector<std::vector<VirtualTerminalCell>> cells;
    detail::AnsiStyleState style;
    CursorPosition cursor;
    std::size_t sync_depth{0};
    bool clear_screen_called{false};
};

namespace {

constexpr std::string_view kBeginSync = "\x1b[?2026h";
constexpr std::string_view kEndSync = "\x1b[?2026l";

} // anonymous namespace

namespace {

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

template <typename T>
void refresh_screen(T& impl) {
    impl.screen.assign(impl.dimensions.rows, {});
    for (std::size_t row = 0; row < impl.cells.size(); ++row) {
        const auto& cells = impl.cells[row];
        std::size_t occupied = 0;
        for (std::size_t column = 0; column < cells.size(); ++column) {
            if (!cells[column].grapheme.empty() || cells[column].continuation) occupied = column + 1;
        }
        auto& line = impl.screen[row];
        for (std::size_t column = 0; column < occupied; ++column) {
            const auto& cell = cells[column];
            if (cell.continuation) continue;
            line += cell.grapheme.empty() ? " " : cell.grapheme;
        }
    }
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
    if (auto result = require_started(*impl_); !result) return std::unexpected(result.error());
    impl_->clear_screen_called = true;
    impl_->cells.assign(
        impl_->dimensions.rows,
        std::vector<VirtualTerminalCell>(impl_->dimensions.columns));
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
    if (position.row >= impl_->dimensions.rows || position.column > impl_->dimensions.columns) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "Virtual Terminal cursor position is outside its dimensions"));
    }
    impl_->cursor = position;
    return {};
}

util::ExpectedVoid VirtualTerminal::set_cursor_visible(bool visible) {
    if (auto result = require_started(*impl_); !result) return std::unexpected(result.error());
    impl_->modes.cursor_visible = visible;
    return {};
}

util::ExpectedVoid VirtualTerminal::inject_input(std::string input) {
    if (!impl_->modes.started || !impl_->input_sink) return {};
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

const std::vector<std::vector<VirtualTerminalCell>>& VirtualTerminal::cells() const {
    return impl_->cells;
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

} // namespace cch::tui

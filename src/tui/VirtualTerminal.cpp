#include <cch/tui/VirtualTerminal.hpp>

#include "tui/TextValidation.hpp"

#include <algorithm>
#include <exception>
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

template <typename T>
void resize_screen(T& impl) {
    impl.screen.resize(impl.dimensions.rows);
    for (auto& line : impl.screen) {
        if (line.size() > impl.dimensions.columns) {
            line.resize(impl.dimensions.columns);
        }
    }
    if (impl.dimensions.rows == 0) {
        impl.cursor = {};
        return;
    }
    impl.cursor.row = std::min(impl.cursor.row, impl.dimensions.rows - 1);
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
    if (auto result = detail::validate_ascii_text(output); !result) {
        return std::unexpected(result.error());
    }
    const auto visible_width = impl_->dimensions.columns - impl_->cursor.column;
    if (output.size() > visible_width) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "TUI component rendered a line wider than its width bound",
            "line width " + std::to_string(output.size()) +
                " exceeds visible width " + std::to_string(visible_width)));
    }

    impl_->output.emplace_back(output);
    if (impl_->cursor.row >= impl_->screen.size()) {
        return {};
    }

    auto& line = impl_->screen[impl_->cursor.row];
    if (line.size() < impl_->cursor.column) {
        line.resize(impl_->cursor.column, ' ');
    }
    if (line.size() < impl_->cursor.column + output.size()) {
        line.resize(impl_->cursor.column + output.size(), ' ');
    }
    line.replace(impl_->cursor.column, output.size(), output);
    impl_->cursor.column += output.size();
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

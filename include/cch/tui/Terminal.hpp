#pragma once

#include <cch/util/Error.hpp>

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>

namespace cch::tui {

struct TerminalDimensions {
    std::size_t columns{80};
    std::size_t rows{24};

    bool operator==(const TerminalDimensions&) const = default;
};

struct CursorPosition {
    std::size_t column{0};
    std::size_t row{0};

    bool operator==(const CursorPosition&) const = default;
};

struct TerminalModeState {
    bool started{false};
    bool raw_input{false};
    bool bracketed_paste{false};
    bool cursor_visible{true};

    bool operator==(const TerminalModeState&) const = default;
};

struct TerminalCapabilities {
    bool synchronized_output{false};
};

using TerminalInputSink = std::move_only_function<void(std::string)>;
using TerminalResizeSink = std::move_only_function<void(TerminalDimensions)>;

class Terminal {
public:
    virtual ~Terminal() = default;

    [[nodiscard]] virtual util::ExpectedVoid start(
        TerminalInputSink input_sink,
        TerminalResizeSink resize_sink) = 0;
    [[nodiscard]] virtual util::ExpectedVoid stop() = 0;
    [[nodiscard]] virtual TerminalDimensions dimensions() const = 0;
    [[nodiscard]] virtual TerminalCapabilities capabilities() const = 0;
    [[nodiscard]] virtual TerminalModeState modes() const = 0;
    [[nodiscard]] virtual util::ExpectedVoid clear_screen() = 0;
    [[nodiscard]] virtual util::ExpectedVoid write(std::string_view output) = 0;
    [[nodiscard]] virtual util::ExpectedVoid set_cursor(CursorPosition position) = 0;
    [[nodiscard]] virtual util::ExpectedVoid set_cursor_visible(bool visible) = 0;
};

} // namespace cch::tui

#pragma once

#include <cch/util/Error.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
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

struct CellRegion {
    std::size_t column{0};
    std::size_t row{0};
    std::size_t columns{0};
    std::size_t rows{0};

    bool operator==(const CellRegion&) const = default;
};

enum class InlineImageProtocol {
    None,
    Kitty,
    ITerm2,
};

struct CellPixelDimensions {
    std::size_t width{9};
    std::size_t height{18};

    bool operator==(const CellPixelDimensions&) const = default;
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
    InlineImageProtocol inline_images{InlineImageProtocol::None};
    std::optional<CellPixelDimensions> cell_pixels{std::nullopt};
};

struct TerminalImageHandle {
    std::uint64_t value{0};

    bool operator==(const TerminalImageHandle&) const = default;
};

/// Borrowed image data consumed synchronously by Terminal::place_image().
struct TerminalImage {
    std::string_view encoded_data{};
    std::string_view mime_type{};
    std::optional<std::string_view> filename{std::nullopt};
    std::size_t pixel_width{0};
    std::size_t pixel_height{0};
    std::uint64_t resource_id{0};
    std::uint64_t revision{0};
    CellRegion region{};
};

/// Delivers raw input bytes. An empty value flushes an incomplete escape sequence
/// after the terminal's ambiguity timeout.
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

    /// Place one validated image inside a TUI-owned absolute cell region.
    [[nodiscard]] virtual util::Expected<TerminalImageHandle> place_image(const TerminalImage& image) = 0;

    /// Remove one image previously returned by place_image(). The region is supplied
    /// so protocols without addressable deletion can clear exactly the owned cells.
    [[nodiscard]] virtual util::ExpectedVoid remove_image(
        TerminalImageHandle handle,
        const CellRegion& region) = 0;

    /// Begin a synchronized update region. Intermediate writes are not displayed
    /// until end_synchronized_update(). Terminals that do not support this
    /// capability may ignore the start/end markers.
    [[nodiscard]] virtual util::ExpectedVoid begin_synchronized_update() = 0;
    [[nodiscard]] virtual util::ExpectedVoid end_synchronized_update() = 0;
};

} // namespace cch::tui

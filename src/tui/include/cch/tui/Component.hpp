#pragma once

#include <cch/tui/Keys.hpp>
#include <cch/tui/Terminal.hpp>
#include <cch/support/Error.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace cch::tui {

using BackgroundHook = std::move_only_function<std::string(std::string)>;

/// Protocol-neutral inline image metadata composed relative to Component output.
/// The TUI root consumes the owned data synchronously and owns physical placement.
struct InlineImageRenderRegion {
    std::uint64_t resource_id{0};
    std::uint64_t revision{0};
    std::string encoded_data{};
    std::string mime_type{};
    std::optional<std::string> filename{std::nullopt};
    std::size_t pixel_width{0};
    std::size_t pixel_height{0};
    std::optional<std::size_t> max_width{std::nullopt};
    std::optional<std::size_t> max_height{std::nullopt};
    std::string fallback_text{};
    CellRegion region{};
};

/// One passive Component frame: validated text backing plus relative image sidecars.
struct RenderResult {
    std::vector<std::string> lines{};
    std::vector<InlineImageRenderRegion> images{};
    std::optional<std::size_t> viewport_height{std::nullopt};
    std::vector<std::string> dock_lines{};
};

/// A width-bounded piece of terminal presentation.
class Component {
public:
    virtual ~Component() = default;

    [[nodiscard]] virtual support::Expected<RenderResult> render(std::size_t width) = 0;
    virtual void invalidate() = 0;
};

/// Outcome of offering a decoded terminal input event to an input handler.
enum class InputAdmissionOutcome {
    /// The handler did not claim the event; propagation continues.
    Unhandled,
    /// The handler claimed the event; propagation stops.
    Consumed,
};

/// An optional Component capability for receiving decoded terminal input.
class InputHandler {
public:
    virtual ~InputHandler() = default;

    [[nodiscard]] virtual InputAdmissionOutcome handle_input(const InputEventVariant& input) = 0;
};

/// An optional Component capability for receiving TUI focus.
class Focusable {
public:
    virtual ~Focusable() = default;

    virtual void set_focused(bool focused) = 0;
    [[nodiscard]] virtual bool focused() const = 0;

    /// Render-time cursor location for IME cursor positioning.
    /// Returns std::nullopt when no cursor should be shown.
    [[nodiscard]] virtual std::optional<CursorPosition> cursor_location() const { return std::nullopt; }
};

/// An optional Component capability for adapting presentation to its viewport.
class ViewportAware {
public:
    virtual ~ViewportAware() = default;

    virtual void set_available_height(std::size_t rows) = 0;
};

} // namespace cch::tui

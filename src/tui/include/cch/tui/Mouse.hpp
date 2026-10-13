#pragma once

#include <cch/tui/Component.hpp>

#include <cstddef>
#include <optional>

namespace cch::tui {

/// Mouse event kind (pi `TuiMouseEventType`).
enum class MouseEventType { Press, Release, Move, Drag, Click, Wheel };

/// Mouse button (pi `TuiMouseButton`).
enum class MouseButton { Left, Middle, Right, None };

/// One normalized cell-based mouse event (pi `TuiMouseEvent`). Coordinates are
/// zero-based; `x`/`y` are local to the receiving component and `screen_x`/
/// `screen_y` are absolute terminal coordinates.
struct MouseEvent {
    MouseEventType type{MouseEventType::Press};
    MouseButton button{MouseButton::None};
    std::ptrdiff_t x{0};
    std::ptrdiff_t y{0};
    std::ptrdiff_t screen_x{0};
    std::ptrdiff_t screen_y{0};
    /// Bounds of the receiving component.
    std::size_t width{1};
    std::size_t height{1};
    bool shift{false};
    bool alt{false};
    bool ctrl{false};
    /// Logical lines; negative values scroll up (pi `wheelDelta`).
    std::optional<std::ptrdiff_t> wheel_delta{std::nullopt};
    /// Consecutive click count when `type` is `Click`.
    std::optional<std::size_t> click_count{std::nullopt};
};

/// One component's answer before a dispatch target is attached (pi
/// `TuiMouseEventResult`).
struct MouseEventResult {
    /// Stop propagation and suppress renderer-level fallback behavior.
    bool handled{false};
    /// Route subsequent drag/release events to this component. Implies handled.
    bool capture{false};
    /// Give keyboard focus to this component. Implies handled.
    bool focus{false};
    /// Explicitly request or suppress a render. Move and release default to
    /// false; press, click, drag, and wheel default to true.
    std::optional<bool> render{std::nullopt};
};

/// The component an event was resolved to, plus the coordinate transform that
/// reaches it (pi `TuiMouseDispatchTarget`).
struct MouseDispatchTarget {
    /// Never null in a resolved result: the dispatcher attaches the component
    /// that answered, or keeps the child a forwarding host resolved to.
    Component* component{nullptr};
    std::ptrdiff_t origin_x{0};
    std::ptrdiff_t origin_y{0};
    std::size_t width{1};
    std::size_t height{1};
};

/// A mouse answer resolved to its target (pi `TuiMouseDispatchResult`).
struct MouseDispatchResult {
    bool handled{true};
    MouseDispatchTarget target{};
    /// Keyboard focus target, which may be a delegating parent container.
    Component* focus_target{nullptr};
    bool capture{false};
    bool focus{false};
    std::optional<bool> render{std::nullopt};
};

/// An optional Component capability for normalized mouse events (pi
/// `Component.handleMouse`).
///
/// A component answers for itself through `handle_mouse`; the dispatcher then
/// attaches it as the event's target. A component that hosts children and
/// forwards the event to them answers through `handle_mouse_forwarded` with the
/// child's already resolved result, and the dispatcher keeps that target so
/// later drag and release events reach the same child.
class MouseHandler {
public:
    virtual ~MouseHandler() = default;

    /// This component's own answer, or nothing when it does not handle the
    /// event. `MouseEventResult` carries only effects; the dispatcher resolves
    /// the target.
    [[nodiscard]] virtual std::optional<MouseEventResult> handle_mouse(const MouseEvent&) { return std::nullopt; }

    /// The resolved answer of a child this component forwarded the event to, or
    /// nothing when no child handled it.
    [[nodiscard]] virtual std::optional<MouseDispatchResult> handle_mouse_forwarded(const MouseEvent&) {
        return std::nullopt;
    }
};

/// Whether an event type renders by default when a handler leaves `render`
/// unset (pi `TuiMouseEventResult.render`).
[[nodiscard]] bool mouse_event_renders_by_default(MouseEventType type) noexcept;

/// Dispatch `event` to `component` and retain the exact target and coordinate
/// transform (pi `dispatchMouseEvent`). Returns `std::nullopt` when the
/// component has no mouse capability or does not handle the event.
[[nodiscard]] std::optional<MouseDispatchResult> dispatch_mouse_event(Component& component, const MouseEvent& event);

/// Recreate local coordinates for a previously dispatched target (pi
/// `retargetMouseEvent`), so a captured component keeps receiving drag and
/// release events at its own coordinates even outside its bounds.
[[nodiscard]] MouseEvent retarget_mouse_event(const MouseEvent& event, const MouseDispatchTarget& target);

} // namespace cch::tui

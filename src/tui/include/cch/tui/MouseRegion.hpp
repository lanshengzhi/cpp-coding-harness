#pragma once

#include <cch/tui/Component.hpp>
#include <cch/tui/Mouse.hpp>

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace cch::tui {

/// One region handler's answer (pi `MouseRegionHandler`).
using MouseRegionHandler = std::function<std::optional<MouseEventResult>(const MouseEvent&)>;

/// Adds mouse handling to an existing component without changing its rendering
/// (pi `components/mouse-region.ts`).
///
/// Mouse events go to the wrapped child first; the region's own handler only
/// answers events the child ignores. Rendering, invalidation, cursor location
/// and containment stay the child's.
class MouseRegion final : public Component, public MouseHandler {
public:
    MouseRegion(std::unique_ptr<Component> child, MouseRegionHandler on_mouse);
    ~MouseRegion() override;

    MouseRegion(const MouseRegion&) = delete;
    MouseRegion& operator=(const MouseRegion&) = delete;

    [[nodiscard]] support::Expected<RenderResult> render(std::size_t width) override;
    void invalidate() override;
    [[nodiscard]] std::optional<CursorPosition> cursor_location() const override;
    [[nodiscard]] bool contains(const Component* target) const override;
    [[nodiscard]] std::optional<MouseEventResult> handle_mouse(const MouseEvent& event) override;
    [[nodiscard]] std::optional<MouseDispatchResult> handle_mouse_forwarded(const MouseEvent& event) override;

    /// The wrapped component.
    [[nodiscard]] Component& child() noexcept { return *child_; }
    [[nodiscard]] const Component& child() const noexcept { return *child_; }

private:
    std::unique_ptr<Component> child_;
    MouseRegionHandler on_mouse_;
};

} // namespace cch::tui

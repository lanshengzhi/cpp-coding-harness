#include <cch/tui/MouseRegion.hpp>

#include <utility>

namespace cch::tui {

MouseRegion::MouseRegion(std::unique_ptr<Component> child, MouseRegionHandler on_mouse)
    : child_(std::move(child)), on_mouse_(std::move(on_mouse)) {}

MouseRegion::~MouseRegion() = default;

support::Expected<RenderResult> MouseRegion::render(std::size_t width) { return child_->render(width); }

void MouseRegion::invalidate() { child_->invalidate(); }

std::optional<CursorPosition> MouseRegion::cursor_location() const { return child_->cursor_location(); }

bool MouseRegion::contains(const Component* target) const { return this == target || child_->contains(target); }

std::optional<MouseEventResult> MouseRegion::handle_mouse(const MouseEvent& event) {
    if (!on_mouse_) return std::nullopt;
    return on_mouse_(event);
}

std::optional<MouseDispatchResult> MouseRegion::handle_mouse_forwarded(const MouseEvent& event) {
    return dispatch_mouse_event(*child_, event);
}

} // namespace cch::tui

#include <cch/tui/Mouse.hpp>

namespace cch::tui {

bool mouse_event_renders_by_default(MouseEventType type) noexcept {
    return type != MouseEventType::Move && type != MouseEventType::Release;
}

std::optional<MouseDispatchResult> dispatch_mouse_event(Component& component, const MouseEvent& event) {
    auto* handler = dynamic_cast<MouseHandler*>(&component);
    if (handler == nullptr) return std::nullopt;

    if (auto forwarded = handler->handle_mouse_forwarded(event)) {
        // The component forwarded the event to a child it hosts. Like a
        // delegating container it routes keys to that child itself, so it keeps
        // keyboard focus: focusing the child directly would leave focus on a
        // detached component once the host removes it (pi `tui.ts:82-86`).
        if (forwarded->focus && dynamic_cast<InputHandler*>(&component) != nullptr) {
            forwarded->focus_target = &component;
        }
        // Each nested host overwrites the field on the way out, so the value is
        // the outermost host's own target: the frame a release can retarget
        // from the raw screen coordinates.
        forwarded->host_target = MouseDispatchTarget{.component = &component,
                .origin_x = event.screen_x - event.x,
                .origin_y = event.screen_y - event.y,
                .width = event.width,
                .height = event.height};
        return forwarded;
    }

    auto answer = handler->handle_mouse(event);
    if (!answer) return std::nullopt;
    if (!answer->handled && !answer->capture && !answer->focus) return std::nullopt;
    return MouseDispatchResult{.handled = true,
            .target = MouseDispatchTarget{.component = &component,
                    .origin_x = event.screen_x - event.x,
                    .origin_y = event.screen_y - event.y,
                    .width = event.width,
                    .height = event.height},
            .focus_target = answer->focus ? &component : nullptr,
            .capture = answer->capture,
            .focus = answer->focus,
            .render = answer->render};
}

MouseEvent retarget_mouse_event(const MouseEvent& event, const MouseDispatchTarget& target) {
    auto retargeted = event;
    retargeted.x = event.screen_x - target.origin_x;
    retargeted.y = event.screen_y - target.origin_y;
    retargeted.width = target.width;
    retargeted.height = target.height;
    return retargeted;
}

} // namespace cch::tui

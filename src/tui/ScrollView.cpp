#include <cch/tui/ScrollView.hpp>

#include <algorithm>
#include <exception>
#include <utility>

namespace cch::tui {

ScrollView::ScrollView(std::unique_ptr<Component> child, ScrollViewOptions options)
    : child_(std::move(child)), follow_end_(options.follow_end), primary_(options.primary),
      overscroll_(options.overscroll), following_end_(options.follow_end) {
    // The viewport owns exactly one child for its whole lifetime: frozen
    // `scroll-view.ts` rejects `addChild`, `removeChild`, and `clear` with
    // errors, so no mutation entry point exists here. A null child is therefore
    // unrepresentable and would fault on the first frame.
    if (!child_) std::terminate();
}

ScrollView::ScrollView(ScrollView&&) noexcept = default;
ScrollView& ScrollView::operator=(ScrollView&&) noexcept = default;
ScrollView::~ScrollView() = default;

std::size_t ScrollView::scroll_top() const noexcept { return scroll_top_; }
bool ScrollView::following_end() const noexcept { return following_end_; }
bool ScrollView::follow_end() const noexcept { return follow_end_; }
bool ScrollView::primary() const noexcept { return primary_; }
ScrollOverscroll ScrollView::overscroll() const noexcept { return overscroll_; }
std::size_t ScrollView::viewport_height() const noexcept { return viewport_height_; }
std::size_t ScrollView::content_height() const noexcept { return content_height_; }

std::size_t ScrollView::max_scroll_top() const noexcept {
    return content_height_ > viewport_height_ ? content_height_ - viewport_height_ : 0;
}

void ScrollView::request_render() {
    if (request_render_) request_render_();
}

void ScrollView::scroll_to(std::ptrdiff_t scroll_top, bool disable_follow) {
    const auto max_scroll_top = this->max_scroll_top();
    const auto requested = scroll_top > 0 ? static_cast<std::size_t>(scroll_top) : std::size_t{0};
    const auto next = std::min(max_scroll_top, requested);
    // A target at the content end disables follow only while `disable_follow`
    // is set (pi `followSuppressedAtEnd`); any other target re-enables it.
    const auto next_follow_suppressed_at_end = disable_follow && next == max_scroll_top;
    const auto next_following_end = !next_follow_suppressed_at_end && follow_end_ && next == max_scroll_top;
    if (next == scroll_top_ && next_following_end == following_end_ &&
            next_follow_suppressed_at_end == follow_suppressed_at_end_) {
        return;
    }
    scroll_top_ = next;
    following_end_ = next_following_end;
    follow_suppressed_at_end_ = next_follow_suppressed_at_end;
    request_render();
}

std::ptrdiff_t ScrollView::scroll_by(std::ptrdiff_t lines) {
    if (lines == 0) return 0;

    const auto max_scroll_top = this->max_scroll_top();
    // A following viewport scrolls from the content end, so the first upward
    // wheel step has to move before the deltas accumulate.
    const auto start = following_end_ ? max_scroll_top : scroll_top_;
    const auto target = static_cast<std::ptrdiff_t>(start) + lines;
    const auto next = target < 0 ? std::size_t{0} : std::min(max_scroll_top, static_cast<std::size_t>(target));
    const auto moved = static_cast<std::ptrdiff_t>(next) - static_cast<std::ptrdiff_t>(start);
    const auto was_following_end = following_end_;
    scroll_top_ = next;
    following_end_ = follow_end_ && next == max_scroll_top;
    follow_suppressed_at_end_ = false;
    if (moved != 0 || following_end_ != was_following_end) request_render();
    // The lines this viewport could not consume belong to an enclosing
    // viewport (pi `scrollBy` return value).
    return lines - moved;
}

void ScrollView::scroll_to_start() {
    const auto settled_following_end = follow_end_ && content_height_ <= viewport_height_;
    const auto changed = scroll_top_ != 0 || following_end_ != settled_following_end;
    scroll_top_ = 0;
    following_end_ = settled_following_end;
    follow_suppressed_at_end_ = false;
    if (changed) request_render();
}

void ScrollView::scroll_to_end() {
    const auto next = max_scroll_top();
    const auto changed = scroll_top_ != next || following_end_ != follow_end_;
    scroll_top_ = next;
    following_end_ = follow_end_;
    follow_suppressed_at_end_ = false;
    if (changed) request_render();
}

void ScrollView::update_layout(
        std::size_t content_height, std::size_t viewport_height, std::function<void()> request_render) {
    content_height_ = content_height;
    viewport_height_ = viewport_height;
    request_render_ = std::move(request_render);
    const auto max_scroll_top = this->max_scroll_top();
    scroll_top_ = following_end_ ? max_scroll_top : std::min(scroll_top_, max_scroll_top);
    // Leaving the end releases a suppressed follow; reaching the end with
    // follow enabled resumes it.
    if (scroll_top_ < max_scroll_top) follow_suppressed_at_end_ = false;
    if (follow_end_ && scroll_top_ == max_scroll_top && !follow_suppressed_at_end_) following_end_ = true;
}

support::Expected<RenderResult> ScrollView::render(std::size_t width) {
    // No scrollbar reserves columns yet (#1001), so the child is measured at
    // the full width and its output is the viewport's content verbatim.
    return child_->render(width);
}

void ScrollView::invalidate() { child_->invalidate(); }

std::optional<CursorPosition> ScrollView::cursor_location() const { return child_->cursor_location(); }

bool ScrollView::contains(const Component* target) const {
    return this == target || (child_ != nullptr && child_->contains(target));
}

} // namespace cch::tui

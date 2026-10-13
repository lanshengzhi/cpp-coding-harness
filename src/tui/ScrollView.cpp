#include <cch/tui/ScrollView.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <format>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

namespace cch::tui {
namespace {

/// The steady one-shot timer a viewport without an injected timer uses: pi's
/// `setTimeout` equivalent. `cancel` waits for an in-flight callback, so a
/// viewport can destroy itself in the timer callback's thread and still never
/// be touched afterwards.
class SteadyScrollbarHideTimer final : public ScrollbarHideTimer {
public:
    SteadyScrollbarHideTimer() = default;
    ~SteadyScrollbarHideTimer() override { cancel(); }

    void start(std::chrono::milliseconds delay, std::function<void()> on_fire) override {
        auto state = std::make_shared<State>();
        state->delay = std::max(std::chrono::milliseconds::zero(), delay);
        state->fire = std::move(on_fire);
        std::shared_ptr<State> previous;
        {
            std::lock_guard lock(mutex_);
            previous = std::exchange(state_, state);
        }
        std::thread([state]() {
            {
                std::lock_guard lock(state->mutex);
                state->thread_id = std::this_thread::get_id();
            }
            std::unique_lock lock(state->mutex);
            const auto expired = !state->condition.wait_for(lock, state->delay, [&]() { return state->stopping; });
            auto fire = expired ? std::move(state->fire) : std::function<void()>{};
            lock.unlock();
            if (fire) fire();
            lock.lock();
            state->active = false;
            lock.unlock();
            state->condition.notify_all();
        }).detach();
        cancel_state(std::move(previous));
    }

    void cancel() override {
        std::shared_ptr<State> state;
        {
            std::lock_guard lock(mutex_);
            state = std::move(state_);
        }
        cancel_state(std::move(state));
    }

private:
    struct State {
        std::mutex mutex;
        std::condition_variable condition;
        std::chrono::milliseconds delay{0};
        std::function<void()> fire;
        std::thread::id thread_id;
        bool stopping{false};
        bool active{true};
    };

    static void cancel_state(std::shared_ptr<State> state) {
        if (!state) return;
        std::unique_lock lock(state->mutex);
        state->stopping = true;
        state->condition.notify_all();
        if (state->thread_id == std::this_thread::get_id()) return;
        state->condition.wait(lock, [&]() { return !state->active; });
    }

    std::mutex mutex_;
    std::shared_ptr<State> state_;
};

[[nodiscard]] std::chrono::milliseconds clamped_hide_delay(std::chrono::milliseconds delay) {
    // pi `Math.max(0, Math.floor(options.scrollbarHideDelayMs ?? 1000))`.
    const auto floored = std::chrono::duration_cast<std::chrono::milliseconds>(delay);
    return floored.count() > 0 ? floored : std::chrono::milliseconds::zero();
}

[[nodiscard]] std::string default_track_style(std::string_view text) { return std::format("\x1b[90m{}\x1b[39m", text); }

[[nodiscard]] std::string default_thumb_style(std::string_view text) { return std::format("\x1b[37m{}\x1b[39m", text); }

} // namespace

ScrollView::ScrollView(std::unique_ptr<Component> child, ScrollViewOptions options)
    : child_(std::move(child)), follow_end_(options.follow_end), primary_(options.primary),
      overscroll_(options.overscroll), scrollbar_(options.scrollbar),
      scrollbar_track_style_(
              options.scrollbar_track_style ? std::move(options.scrollbar_track_style) : default_track_style),
      scrollbar_thumb_style_(
              options.scrollbar_thumb_style ? std::move(options.scrollbar_thumb_style) : default_thumb_style),
      scrollbar_hide_delay_(clamped_hide_delay(options.scrollbar_hide_delay)),
      scrollbar_hide_timer_(std::move(options.scrollbar_hide_timer)), following_end_(options.follow_end) {
    // The viewport owns exactly one child for its whole lifetime: frozen
    // `scroll-view.ts` rejects `addChild`, `removeChild`, and `clear` with
    // errors, so no mutation entry point exists here. A null child is therefore
    // unrepresentable and would fault on the first frame.
    if (!child_) std::terminate();
}

ScrollView::ScrollView(ScrollView&&) noexcept = default;
ScrollView& ScrollView::operator=(ScrollView&&) noexcept = default;
ScrollView::~ScrollView() {
    // A pending hide captures this viewport; the timer must not call back into
    // a destroyed one (pi's `clearTimeout` in `hideTransientScrollbar`).
    hide_transient_scrollbar();
}

std::size_t ScrollView::scroll_top() const noexcept { return scroll_top_; }
bool ScrollView::following_end() const noexcept { return following_end_; }
bool ScrollView::follow_end() const noexcept { return follow_end_; }
bool ScrollView::primary() const noexcept { return primary_; }
ScrollOverscroll ScrollView::overscroll() const noexcept { return overscroll_; }
std::size_t ScrollView::viewport_height() const noexcept { return viewport_height_; }
std::size_t ScrollView::content_height() const noexcept { return content_height_; }
ScrollbarMode ScrollView::scrollbar() const noexcept { return scrollbar_; }

bool ScrollView::is_scrollbar_visible() const noexcept {
    if (scrollbar_ == ScrollbarMode::Always) return viewport_height_ > 0;
    return scrollbar_ == ScrollbarMode::Auto && content_height_ > viewport_height_ && transient_scrollbar_visible_;
}

bool ScrollView::is_scrollbar_active() const noexcept { return scrollbar_active_; }

std::size_t ScrollView::content_width(std::size_t width) const noexcept {
    return scrollbar_ == ScrollbarMode::Always && width > 1 ? width - 1 : width;
}

std::string ScrollView::scrollbar_track_style(std::string_view text) const { return scrollbar_track_style_(text); }

std::string ScrollView::scrollbar_thumb_style(std::string_view text) const { return scrollbar_thumb_style_(text); }

void ScrollView::set_scrollbar(ScrollbarMode scrollbar) {
    if (scrollbar == scrollbar_) return;
    scrollbar_ = scrollbar;
    if (scrollbar != ScrollbarMode::Auto) {
        hide_transient_scrollbar();
    } else if (scrollbar_active_) {
        mark_scrollbar_activity();
    }
    request_render();
}

void ScrollView::set_scrollbar_active(bool active) {
    if (active == scrollbar_active_) return;
    scrollbar_active_ = active;
    mark_scrollbar_activity();
    request_render();
}

void ScrollView::mark_scrollbar_activity() {
    if (scrollbar_ != ScrollbarMode::Auto || content_height_ <= viewport_height_) return;
    transient_scrollbar_visible_ = true;
    if (scrollbar_hide_armed_) {
        scrollbar_hide_armed_ = false;
        if (scrollbar_hide_timer_) scrollbar_hide_timer_->cancel();
    }
    // An active bar is held open by the pointer: no hide is scheduled until the
    // pointer leaves and marks activity again (pi `markScrollbarActivity`).
    if (scrollbar_active_) return;
    if (!scrollbar_hide_timer_) scrollbar_hide_timer_ = std::make_shared<SteadyScrollbarHideTimer>();
    scrollbar_hide_armed_ = true;
    scrollbar_hide_timer_->start(scrollbar_hide_delay_, [this] {
        scrollbar_hide_armed_ = false;
        transient_scrollbar_visible_ = false;
        request_render();
    });
}

void ScrollView::hide_transient_scrollbar() {
    transient_scrollbar_visible_ = false;
    if (!scrollbar_hide_armed_) return;
    scrollbar_hide_armed_ = false;
    if (scrollbar_hide_timer_) scrollbar_hide_timer_->cancel();
}

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
    const auto moved = next != scroll_top_;
    scroll_top_ = next;
    following_end_ = next_following_end;
    follow_suppressed_at_end_ = next_follow_suppressed_at_end;
    if (moved) mark_scrollbar_activity();
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
    if (moved != 0) mark_scrollbar_activity();
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
    if (changed) {
        mark_scrollbar_activity();
        request_render();
    }
}

void ScrollView::scroll_to_end() {
    const auto next = max_scroll_top();
    const auto changed = scroll_top_ != next || following_end_ != follow_end_;
    scroll_top_ = next;
    following_end_ = follow_end_;
    follow_suppressed_at_end_ = false;
    if (changed) {
        mark_scrollbar_activity();
        request_render();
    }
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
    // Content that fits hides a transient bar and its pending timer
    // (pi `updateLayout`).
    if (content_height_ <= viewport_height_) hide_transient_scrollbar();
}

support::Expected<RenderResult> ScrollView::render(std::size_t width) {
    // The viewport's own render is the child at the width the scrollbar leaves
    // (`content_width`), before the frame engine reserves the column and paints
    // the bar (pi `ScrollView.render`).
    return child_->render(content_width(width));
}

void ScrollView::invalidate() { child_->invalidate(); }

std::optional<CursorPosition> ScrollView::cursor_location() const { return child_->cursor_location(); }

bool ScrollView::contains(const Component* target) const {
    return this == target || (child_ != nullptr && child_->contains(target));
}

} // namespace cch::tui

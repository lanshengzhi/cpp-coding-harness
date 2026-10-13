#pragma once

#include <cch/tui/Component.hpp>

#include <cch/support/Error.hpp>

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace cch::tui {

class AltScreen;

namespace detail {
/// The engine's allocation and painting pass (defined in `tui/Layout.cpp`); it
/// reads this viewport's node data and reports the frame it allocated.
class LayoutBuilder;
} // namespace detail

/// pi `ScrollViewOptions.overscroll`: whether a scroll that reaches this
/// viewport's limit may still be consumed by an enclosing viewport.
enum class ScrollOverscroll { Chain, Contain };

/// pi `ScrollViewScrollbar`: how the viewport presents its scrollbar column.
///
/// The enumerators are C++ spellings of the persisted product values
/// `"hidden"`, `"auto"`, and `"always"`; the persisted text itself is the
/// setting schema's (#1020), not this toolkit type's.
enum class ScrollbarMode { Hidden, Auto, Always };

/// The one-shot timer that hides a transient (`auto`) scrollbar (pi
/// `scrollbarHideTimer`).
///
/// The viewport starts it when scroll activity reveals the bar and cancels it
/// when activity is followed by another reveal or by a mode change, so an
/// implementation must fire at most once per `start`, never after `cancel`, and
/// never before `start` returns (pi `setTimeout` fires on a later turn).
/// `cancel` may be called from the viewport's destructor and from inside the
/// timer callback, so it must return only once no callback is running, and it
/// must not invoke `on_fire` after `cancel` returns.
class ScrollbarHideTimer {
public:
    ScrollbarHideTimer() = default;
    ScrollbarHideTimer(const ScrollbarHideTimer&) = delete;
    ScrollbarHideTimer& operator=(const ScrollbarHideTimer&) = delete;
    virtual ~ScrollbarHideTimer() = default;

    virtual void start(std::chrono::milliseconds delay, std::function<void()> on_fire) = 0;
    virtual void cancel() = 0;
};

/// ScrollView constructor options (pi `ScrollViewOptions`).
///
/// The frozen `axis` option only ever admits `"vertical"`, so the type has no
/// spelling for it.
struct ScrollViewOptions {
    /// Follow the content end while rows are appended (pi `follow: "end"`).
    bool follow_end{false};
    /// Prefer this viewport for navigation and programmatic scrolling
    /// (pi `primary`).
    bool primary{false};
    ScrollOverscroll overscroll{ScrollOverscroll::Chain};
    /// pi `scrollbar`: a reusable viewport defaults to hidden; the product's
    /// `auto` default belongs to its own viewport (#1020).
    ScrollbarMode scrollbar{ScrollbarMode::Hidden};
    /// pi `scrollbarTrackStyle`: wraps one track cell (`│`). The default is
    /// pi's dim gray.
    std::function<std::string(std::string_view)> scrollbar_track_style{};
    /// pi `scrollbarThumbStyle`: wraps one thumb cell (`┃`, or `█` while
    /// active). The default is pi's white.
    std::function<std::string(std::string_view)> scrollbar_thumb_style{};
    /// pi `scrollbarHideDelayMs`: how long an `auto` scrollbar stays after
    /// scroll activity. Negative values clamp to zero, as the frozen
    /// `Math.max(0, Math.floor(...))` does.
    std::chrono::milliseconds scrollbar_hide_delay{1000};
    /// The hide timer. A viewport without one creates a steady timer when the
    /// first transient reveal needs it, so a hidden viewport never starts a
    /// thread; deterministic tests inject a timer they control.
    std::shared_ptr<ScrollbarHideTimer> scrollbar_hide_timer{};
};

/// A one-child viewport over a taller component (pi `ScrollView`).
///
/// The viewport owns its single child; the frame engine allocates the child
/// unbounded, reports the content and viewport heights through `update_layout`,
/// and paints the visible window. On its own (`render(width)`) the viewport only
/// forwards the child's output, padding it back to `width` when `always` reserves
/// a column; intrinsic stack measurement therefore includes that reserved cell.
///
/// The frozen scroll algebra is preserved exactly: the scroll top clamps to
/// `[0, content_height - viewport_height]`, `scroll_by` returns the lines it
/// could not consume so an enclosing viewport receives them, and a follow-end
/// viewport that was scrolled away from the end stays away while rows are
/// appended until it reaches the end again.
class ScrollView final : public Component {
public:
    explicit ScrollView(std::unique_ptr<Component> child, ScrollViewOptions options = {});
    ScrollView(ScrollView&&) noexcept;
    ScrollView& operator=(ScrollView&&) noexcept;
    ~ScrollView() override;

    ScrollView(const ScrollView&) = delete;
    ScrollView& operator=(const ScrollView&) = delete;

    [[nodiscard]] std::size_t scroll_top() const noexcept;
    /// Whether appended content moves the viewport (pi `isFollowingEnd`).
    [[nodiscard]] bool following_end() const noexcept;
    [[nodiscard]] bool follow_end() const noexcept;
    [[nodiscard]] bool primary() const noexcept;
    [[nodiscard]] ScrollOverscroll overscroll() const noexcept;
    [[nodiscard]] std::size_t viewport_height() const noexcept;
    [[nodiscard]] std::size_t content_height() const noexcept;
    [[nodiscard]] ScrollbarMode scrollbar() const noexcept;
    /// Whether the frame paints a scrollbar column: `always` needs a viewport,
    /// `auto` needs overflowing content and a transient reveal, `hidden` never
    /// (pi `isScrollbarVisible`).
    [[nodiscard]] bool is_scrollbar_visible() const noexcept;
    /// Whether a pointer is on the scrollbar (pi `isScrollbarActive`): an
    /// active bar keeps its transient reveal and draws the expanded `█` thumb.
    [[nodiscard]] bool is_scrollbar_active() const noexcept;

    /// Replace the scrollbar mode at runtime (pi `setScrollbar`). Leaving `auto`
    /// hides a transient bar; entering it while active reveals one.
    void set_scrollbar(ScrollbarMode scrollbar);
    /// The width the viewport offers its child: `always` reserves the last
    /// column when there is more than one, everything else keeps the width
    /// (pi `getContentWidth`).
    [[nodiscard]] std::size_t content_width(std::size_t width) const noexcept;
    /// Report pointer presence on the scrollbar (pi `setScrollbarActive`).
    void set_scrollbar_active(bool active);

    /// One styled track cell (pi `scrollbarTrackStyle`).
    [[nodiscard]] std::string scrollbar_track_style(std::string_view text) const;
    /// One styled thumb cell (pi `scrollbarThumbStyle`).
    [[nodiscard]] std::string scrollbar_thumb_style(std::string_view text) const;

    /// Scroll to an absolute row, clamped to the content (pi `scrollTo`).
    /// `disable_follow` keeps follow-end disabled while the target is the
    /// content end (pi `ScrollViewScrollToOptions.disableFollow`).
    void scroll_to(std::ptrdiff_t scroll_top, bool disable_follow = false);
    /// Scroll by a signed row delta and return the lines this viewport did not
    /// consume, which is the delta an enclosing viewport receives
    /// (pi `scrollBy`).
    [[nodiscard]] std::ptrdiff_t scroll_by(std::ptrdiff_t lines);
    void scroll_to_start();
    void scroll_to_end();

    /// Frame measurement hook (pi `updateLayout`): the engine reports the
    /// heights it allocated and the callback that requests the next render.
    void update_layout(
            std::size_t content_height, std::size_t viewport_height, std::function<void()> request_render = {});

    [[nodiscard]] support::Expected<RenderResult> render(std::size_t width) override;
    void invalidate() override;
    [[nodiscard]] std::optional<CursorPosition> cursor_location() const override;
    [[nodiscard]] bool contains(const Component* target) const override;

private:
    friend class AltScreen;
    friend class detail::LayoutBuilder;

    [[nodiscard]] std::weak_ptr<void> lifetime_token() const noexcept;
    [[nodiscard]] std::size_t max_scroll_top() const noexcept;
    void request_render();
    /// Reveal the transient bar and (re)arm the hide timer (pi
    /// `markScrollbarActivity`).
    void mark_scrollbar_activity();
    /// Hide the transient bar and cancel its timer (pi
    /// `hideTransientScrollbar`).
    void hide_transient_scrollbar();

    std::unique_ptr<Component> child_;
    bool follow_end_{false};
    bool primary_{false};
    ScrollOverscroll overscroll_{ScrollOverscroll::Chain};
    ScrollbarMode scrollbar_{ScrollbarMode::Hidden};
    std::function<std::string(std::string_view)> scrollbar_track_style_{};
    std::function<std::string(std::string_view)> scrollbar_thumb_style_{};
    std::chrono::milliseconds scrollbar_hide_delay_{1000};
    std::shared_ptr<ScrollbarHideTimer> scrollbar_hide_timer_{};
    /// Expires with this viewport so roots can safely track non-owning hover.
    std::shared_ptr<void> lifetime_token_{std::make_shared<int>(0)};
    /// Whether `scrollbar_hide_timer_` holds a pending hide (pi's timer handle).
    bool scrollbar_hide_armed_{false};
    bool transient_scrollbar_visible_{false};
    bool scrollbar_active_{false};
    /// pi's `currentScrollTop`; `update_layout` keeps it inside the content.
    std::size_t scroll_top_{0};
    std::size_t content_height_{0};
    std::size_t viewport_height_{0};
    bool following_end_{false};
    /// A `scroll_to` that reached the content end with `disable_follow` set:
    /// follows again only after the viewport leaves the end (pi
    /// `followSuppressedAtEnd`).
    bool follow_suppressed_at_end_{false};
    std::function<void()> request_render_{};
};

} // namespace cch::tui

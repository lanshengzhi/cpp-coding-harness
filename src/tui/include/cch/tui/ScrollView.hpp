#pragma once

#include <cch/tui/Component.hpp>

#include <cch/support/Error.hpp>

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>

namespace cch::tui {

namespace detail {
/// The engine's allocation and painting pass (defined in `tui/Layout.cpp`); it
/// reads this viewport's node data and reports the frame it allocated.
class LayoutBuilder;
} // namespace detail

/// pi `ScrollViewOptions.overscroll`: whether a scroll that reaches this
/// viewport's limit may still be consumed by an enclosing viewport.
enum class ScrollOverscroll { Chain, Contain };

/// ScrollView constructor options (pi `ScrollViewOptions`).
///
/// The frozen `axis` option only ever admits `"vertical"`, so the type has no
/// spelling for it; scrollbar presentation is #1001's surface.
struct ScrollViewOptions {
    /// Follow the content end while rows are appended (pi `follow: "end"`).
    bool follow_end{false};
    /// Prefer this viewport for navigation and programmatic scrolling
    /// (pi `primary`).
    bool primary{false};
    ScrollOverscroll overscroll{ScrollOverscroll::Chain};
};

/// A one-child viewport over a taller component (pi `ScrollView`).
///
/// The viewport owns its single child; the frame engine allocates the child
/// unbounded, reports the content and viewport heights through `update_layout`,
/// and paints the visible window. On its own (`render(width)`) the viewport only
/// forwards the child's output, so a standalone render is the whole content.
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
    friend class detail::LayoutBuilder;

    [[nodiscard]] std::size_t max_scroll_top() const noexcept;
    void request_render();

    std::unique_ptr<Component> child_;
    bool follow_end_{false};
    bool primary_{false};
    ScrollOverscroll overscroll_{ScrollOverscroll::Chain};
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

#pragma once

#include <cch/tui/Component.hpp>

#include <cch/support/Error.hpp>

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace cch::tui {

class HStack;
class ScrollView;
class VStack;

} // namespace cch::tui

namespace cch::tui::detail {

/// Cells of one allocated box (pi `LayoutRect`).
struct LayoutRect {
    std::ptrdiff_t x{0};
    std::ptrdiff_t y{0};
    std::size_t width{0};
    std::size_t height{0};
};

/// One laid-out component (pi `LayoutBox`): its allocated rect, the clip it
/// inherited from its ancestors, and its laid-out children.
struct LayoutBox {
    Component* component{nullptr};
    LayoutRect rect{};
    LayoutRect clip{};
    std::vector<LayoutBox> children{};
    /// Whether this box owns the lines it renders instead of laying out
    /// children (pi `LayoutBox.lines !== undefined`).
    bool leaf{false};
    /// Rows dropped from the top of this leaf's output to keep its cursor row
    /// visible inside `rect` (pi `lineOffset`).
    std::size_t line_offset{0};
    /// The scroll viewport this box allocates, when its component is a
    /// `ScrollView` (pi `LayoutBox.scrollView`): the box clips and translates
    /// its child to the window the viewport shows.
    ScrollView* scroll_view{nullptr};
};

/// One layout frame (pi `LayoutFrame`): the box tree plus the composed rows,
/// image placements, and cursor it produces.
struct LayoutFrame {
    LayoutBox root{};
    std::size_t width{0};
    std::size_t height{0};
    std::vector<std::string> lines{};
    std::vector<InlineImageRenderRegion> images{};
    std::optional<CursorPosition> cursor{};
    /// The viewport a navigation key or wheel delta acts on first (pi
    /// `LayoutFrame.primaryScrollView`): the first `primary` viewport, or the
    /// first viewport of the frame when none declares itself primary.
    ScrollView* primary_scroll_view{nullptr};
};

/// The constrained layout authority (pi `renderLayoutFrame`): allocate a
/// component tree into an exact `width` x `height` cell frame and compose it.
///
/// A stack node (vertical or horizontal) is laid out through its entries, a
/// scroll viewport allocates its one child unbounded and paints the window it
/// shows, and every other component is a leaf whose output is rendered at the
/// frame width, cropped to its allocated rect, and translated into the frame.
/// Non-positive dimensions clamp to one cell, matching the frozen viewport.
class LayoutEngine {
public:
    /// `request_render` is the callback a viewport stores to ask for the next
    /// frame when its scroll position changes during this one (pi
    /// `LayoutContext.requestRender`).
    [[nodiscard]] static support::Expected<LayoutFrame> render(
            Component& root, std::size_t width, std::size_t height, std::function<void()> request_render = {});
};

/// The scroll viewports whose rect contains a frame cell, deepest first (pi
/// `getScrollViewsAt`). A caller offers a scroll delta to the first viewport and
/// passes what it does not consume outward, stopping at
/// `ScrollOverscroll::Contain` (pi `TuiAltScreen.routeWheel`).
[[nodiscard]] std::vector<ScrollView*> scroll_views_at(const LayoutFrame& frame, std::ptrdiff_t x, std::ptrdiff_t y);

/// Implementation of the engine's allocation and painting pass, defined in
/// `tui/Layout.cpp`. It is the seam through which the engine reads a stack
/// node's entries and options (pi `LAYOUT_NODE`).
class LayoutBuilder;

} // namespace cch::tui::detail

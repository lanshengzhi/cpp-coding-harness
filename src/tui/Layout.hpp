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
    /// The content rendered by this viewport before its scroll offset is
    /// applied (pi `LayoutBox.scrollContentLines`).
    std::vector<std::string> scroll_content_lines{};
    /// Whether the content came from a terminal-visible leaf rather than a
    /// composite stack whose selection source is the composed screen.
    bool has_scroll_content_lines{false};
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

/// The visual hit path of a frame cell, deepest first (pi `getLayoutBoxesAt`):
/// every box whose inherited clip contains the cell, ordered as pi orders the
/// candidates for pointer targeting. A caller offers a normalized mouse event
/// to the first box whose component has a mouse capability and resolves the
/// local coordinates from that box's rect (pi `TuiAltScreen.dispatchMouseToLayout`).
[[nodiscard]] std::vector<const LayoutBox*> layout_boxes_at(
        const LayoutFrame& frame, std::ptrdiff_t x, std::ptrdiff_t y);

/// The scroll viewports whose rect contains a frame cell, deepest first (pi
/// `getScrollViewsAt`). A caller offers a scroll delta to the first viewport and
/// passes what it does not consume outward, stopping at
/// `ScrollOverscroll::Contain` (pi `TuiAltScreen.routeWheel`).
[[nodiscard]] std::vector<ScrollView*> scroll_views_at(const LayoutFrame& frame, std::ptrdiff_t x, std::ptrdiff_t y);

/// The painted geometry of a viewport's scrollbar column (pi
/// `ScrollbarGeometry`): the frame column it occupies, the rows the track
/// covers, and the thumb's position and size inside it.
struct ScrollbarGeometry {
    std::ptrdiff_t column{0};
    std::ptrdiff_t track_top{0};
    std::size_t track_height{0};
    std::ptrdiff_t thumb_top{0};
    std::size_t thumb_height{0};
    std::size_t max_scroll_top{0};
};

/// The scrollbar geometry a painted box presents, or nothing when the box is
/// not a viewport, has no cells, or shows no bar (pi `getScrollbarGeometry`).
/// `include_hidden_auto` measures the track a hidden `auto` bar would reveal
/// under the pointer, which is what a hover hit test needs.
[[nodiscard]] std::optional<ScrollbarGeometry> get_scrollbar_geometry(
        const LayoutBox& box, bool include_hidden_auto = false);

/// The box a viewport was allocated in a frame (pi `getScrollViewBox`), which
/// pairs a `scroll_views_at` candidate with the geometry it presents.
[[nodiscard]] const LayoutBox* get_scroll_view_box(const LayoutFrame& frame, const ScrollView* viewport);

/// Implementation of the engine's allocation and painting pass, defined in
/// `tui/Layout.cpp`. It is the seam through which the engine reads a stack
/// node's entries and options (pi `LAYOUT_NODE`).
class LayoutBuilder;

} // namespace cch::tui::detail

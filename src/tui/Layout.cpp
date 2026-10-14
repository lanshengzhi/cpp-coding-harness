#include "tui/Layout.hpp"

#include "tui/LineCompositor.hpp"
#include "tui/RenderUtils.hpp"
#include "tui/StackLayout.hpp"
#include "tui/UnicodeWidth.hpp"

#include <cch/tui/HStack.hpp>
#include <cch/tui/ScrollView.hpp>
#include <cch/tui/TerminalImage.hpp>
#include <cch/tui/Utils.hpp>
#include <cch/tui/VStack.hpp>
#include <cch/support/Error.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <format>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace cch::tui::detail {
namespace {

/// Absolute-position intersection in frame cells (pi `layout.ts` `intersect`).
[[nodiscard]] LayoutRect intersect(const LayoutRect& a, const LayoutRect& b) {
    const auto x = std::max(a.x, b.x);
    const auto y = std::max(a.y, b.y);
    const auto right = std::min(a.x + static_cast<std::ptrdiff_t>(a.width), b.x + static_cast<std::ptrdiff_t>(b.width));
    const auto bottom =
            std::min(a.y + static_cast<std::ptrdiff_t>(a.height), b.y + static_cast<std::ptrdiff_t>(b.height));
    return LayoutRect{.x = x,
            .y = y,
            .width = right > x ? static_cast<std::size_t>(right - x) : 0,
            .height = bottom > y ? static_cast<std::size_t>(bottom - y) : 0};
}

/// One component's output for one width inside one frame (pi
/// `LayoutContext.renderCache`): a component renders once per width per frame,
/// and the cache cannot outlive the frame. The durable cache is #1002.
using FrameRenderCache = std::unordered_map<const Component*, std::unordered_map<std::size_t, RenderResult>>;

struct LayoutContext {
    LayoutViewport viewport{};
    FrameRenderCache render_cache{};
    /// Stored by every scroll viewport this frame allocates (pi
    /// `LayoutContext.requestRender`).
    std::function<void()> request_render{};
    /// The first `primary` viewport, or the first viewport of the frame
    /// (pi `LayoutContext.primaryScrollView`).
    ScrollView* primary_scroll_view{nullptr};
};

/// Move a laid-out subtree by whole rows (pi `translateBox`): a scroll viewport
/// lays its child out at the previous scroll position and then shifts it to the
/// position the child's own `update_layout` settled on.
void translate_box(LayoutBox& box, std::ptrdiff_t delta_y) {
    box.rect.y += delta_y;
    for (auto& child : box.children)
        translate_box(child, delta_y);
}

/// Re-clip a laid-out subtree against an ancestor's clip (pi `updateClips`): a
/// scroll viewport's child is laid out against the viewport's incoming clip and
/// then clipped to the window, so the frame paints only the visible rows.
void update_clips(LayoutBox& box, const LayoutRect& parent_clip) {
    box.clip = intersect(parent_clip, box.rect);
    for (auto& child : box.children)
        update_clips(child, box.clip);
}

[[nodiscard]] bool contains_point(const LayoutRect& rect, std::ptrdiff_t x, std::ptrdiff_t y) {
    return x >= rect.x && x < rect.x + static_cast<std::ptrdiff_t>(rect.width) && y >= rect.y &&
           y < rect.y + static_cast<std::ptrdiff_t>(rect.height);
}

void collect_scroll_views(const LayoutBox& box,
        std::ptrdiff_t x,
        std::ptrdiff_t y,
        std::size_t depth,
        std::vector<std::pair<ScrollView*, std::size_t>>& found) {
    if (!contains_point(box.clip, x, y)) return;
    if (box.scroll_view != nullptr && contains_point(box.rect, x, y)) found.emplace_back(box.scroll_view, depth);
    for (const auto& child : box.children)
        collect_scroll_views(child, x, y, depth + 1, found);
}

void collect_boxes_at(const LayoutBox& box,
        std::ptrdiff_t x,
        std::ptrdiff_t y,
        std::size_t depth,
        std::vector<std::pair<const LayoutBox*, std::size_t>>& found) {
    if (!contains_point(box.clip, x, y)) return;
    found.emplace_back(&box, depth);
    for (const auto& child : box.children)
        collect_boxes_at(child, x, y, depth + 1, found);
}

/// The box a viewport was allocated (pi `getScrollbarTargetAt` pairs every
/// `getScrollViewsAt` candidate with its own box).
[[nodiscard]] const LayoutBox* find_scroll_box(const LayoutBox& box, const ScrollView* viewport) {
    if (box.scroll_view == viewport) return &box;
    for (const auto& child : box.children) {
        if (const auto* found = find_scroll_box(child, viewport); found != nullptr) return found;
    }
    return nullptr;
}

/// The cell range one grapheme covers (pi `getGraphemeCellRange`): the cell
/// columns a column sits in, or nothing when the line has no grapheme there.
struct GraphemeCellRange {
    std::ptrdiff_t start{0};
    std::ptrdiff_t end{0};
};

[[nodiscard]] std::optional<GraphemeCellRange> get_grapheme_cell_range(std::string_view line, std::ptrdiff_t column) {
    auto tokens = tokenize_terminal_output(line);
    if (!tokens) return std::nullopt;
    auto current = std::ptrdiff_t{0};
    for (const auto& token : *tokens) {
        if (token.kind != TerminalTokenKind::Grapheme) continue;
        const auto width = static_cast<std::ptrdiff_t>(token.width);
        if (width > 0 && column >= current && column < current + width) {
            return GraphemeCellRange{.start = current, .end = current + width};
        }
        current += width;
    }
    return std::nullopt;
}

/// The active background of a line prefix (pi `getActiveBackgroundAnsi`): the
/// last background SGR the prefix set, re-emitted on its own so a replaced cell
/// keeps the colour an overlay scrollbar must not erase.
[[nodiscard]] std::string active_background_ansi(std::string_view prefix) {
    AnsiStyleState style;
    std::size_t index = 0;
    while (index < prefix.size()) {
        const auto code = extract_ansi_code(prefix, index);
        if (!code) break;
        style.process_ansi(code->code);
        index += code->length;
    }
    return style.bg_color.empty() ? std::string{} : std::format("\x1b[{}m", style.bg_color);
}

/// Splice one scrollbar glyph into a painted row (pi `replaceScrollbarCell`):
/// the whole grapheme the column sits in is replaced, the cell keeps the target
/// background when the bar floats over the content, and the row is re-sliced
/// through the column so the surrounding styles survive.
[[nodiscard]] support::Expected<std::string> replace_scrollbar_cell(std::string_view line,
        std::ptrdiff_t column,
        std::size_t total_width,
        std::string_view replacement,
        bool preserve_target_background) {
    if (is_image_line(line)) return std::string{line};

    const auto range = get_grapheme_cell_range(line, column);
    const auto start = range ? range->start : column;
    const auto end = range ? range->end : column + 1;
    auto before = slice_by_column(line, 0, static_cast<std::size_t>(start), true);
    if (!before) return std::unexpected(before.error());
    auto target = slice_by_column(line, static_cast<std::size_t>(start), static_cast<std::size_t>(end - start), true);
    if (!target) return std::unexpected(target.error());
    const auto trailing = static_cast<std::ptrdiff_t>(total_width) - end;
    auto after = slice_by_column(
            line, static_cast<std::size_t>(end), static_cast<std::size_t>(std::max<std::ptrdiff_t>(0, trailing)), true);
    if (!after) return std::unexpected(after.error());

    std::string target_prefix;
    std::size_t index = 0;
    while (index < target->size()) {
        const auto code = extract_ansi_code(*target, index);
        if (!code) break;
        target_prefix += code->code;
        index += code->length;
    }
    const auto before_width = static_cast<std::ptrdiff_t>(measure_visible_width(*before).width);
    const auto before_padding =
            std::string(static_cast<std::size_t>(std::max<std::ptrdiff_t>(0, start - before_width)), ' ');
    const auto cell_padding_before =
            std::string(static_cast<std::size_t>(std::max<std::ptrdiff_t>(0, column - start)), ' ');
    const auto cell_padding_after =
            std::string(static_cast<std::size_t>(std::max<std::ptrdiff_t>(0, end - column - 1)), ' ');
    std::string target_style = std::format("\x1b[0m{}", kOsc8LinkClose);
    if (preserve_target_background) target_style += active_background_ansi(target_prefix);

    std::string result;
    result.reserve(before->size() + target_style.size() + replacement.size() + after->size());
    result += *before;
    result += before_padding;
    result += target_style;
    result += cell_padding_before;
    result += replacement;
    result += cell_padding_after;
    result += *after;
    return result;
}

/// Paint a viewport's scrollbar column (pi `paintScrollbar`): the track covers
/// the viewport rect, the thumb's proportional slice of it is drawn with the
/// expanded glyph while a pointer is on the bar, and only an `always` bar has
/// reserved the column it overwrites.
[[nodiscard]] support::ExpectedVoid paint_scrollbar(const LayoutBox& box, LayoutFrame& frame) {
    const auto geometry = get_scrollbar_geometry(box);
    if (!geometry) return {};
    auto* scroll = box.scroll_view;
    for (std::size_t offset = 0; offset < geometry->track_height; ++offset) {
        const auto row = geometry->track_top + static_cast<std::ptrdiff_t>(offset);
        const auto track_last = box.clip.y + static_cast<std::ptrdiff_t>(box.clip.height);
        if (row < box.clip.y || row >= track_last || row < 0 ||
                row >= static_cast<std::ptrdiff_t>(frame.lines.size())) {
            continue;
        }
        const auto thumb_last = geometry->thumb_top + static_cast<std::ptrdiff_t>(geometry->thumb_height);
        const auto is_thumb = row >= geometry->thumb_top && row < thumb_last;
        const auto replacement =
                is_thumb ? scroll->scrollbar_thumb_style(scroll->is_scrollbar_active() ? "\u2588" : "\u2503")
                         : scroll->scrollbar_track_style("\u2502");
        auto replaced = replace_scrollbar_cell(frame.lines[static_cast<std::size_t>(row)],
                geometry->column,
                frame.width,
                replacement,
                scroll->scrollbar() != ScrollbarMode::Always);
        if (!replaced) return std::unexpected(replaced.error());
        frame.lines[static_cast<std::size_t>(row)] = std::move(*replaced);
    }
    return {};
}

} // namespace

/// The layout engine's implementation: allocation and painting, with the stack
/// node internals the engine needs (pi `LAYOUT_NODE`). Defined here only.
class LayoutBuilder {
public:
    [[nodiscard]] static support::Expected<LayoutFrame> build(
            Component& root, std::size_t width, std::size_t height, std::function<void()> request_render);

private:
    [[nodiscard]] static support::Expected<std::reference_wrapper<const RenderResult>> render_cached(
            LayoutContext& context, Component& component, std::size_t width);
    [[nodiscard]] static support::Expected<std::size_t> measure_height(
            LayoutContext& context, Component& component, std::size_t width);
    [[nodiscard]] static support::Expected<std::size_t> measure_width(
            LayoutContext& context, Component& component, std::size_t width);
    /// Passive views over a stack's live entries, in stack order (pi
    /// `LAYOUT_NODE.entries`).
    [[nodiscard]] static std::vector<StackEntry> stack_entry_views(const VStack& stack);
    [[nodiscard]] static std::vector<StackEntry> stack_entry_views(const HStack& stack);
    [[nodiscard]] static support::Expected<LayoutBox> layout_component(LayoutContext& context,
            Component& component,
            std::ptrdiff_t x,
            std::ptrdiff_t y,
            std::size_t width,
            std::optional<std::size_t> height,
            const LayoutRect& clip);
    [[nodiscard]] static support::ExpectedVoid paint_box(
            LayoutContext& context, const LayoutBox& box, LayoutFrame& frame);
};

support::Expected<std::reference_wrapper<const RenderResult>> LayoutBuilder::render_cached(
        LayoutContext& context, Component& component, std::size_t width) {
    const auto safe_width = std::max<std::size_t>(1, width);
    auto& widths = context.render_cache[&component];
    if (const auto found = widths.find(safe_width); found != widths.end()) return std::cref(found->second);
    auto rendered = component.render(safe_width);
    if (!rendered) return std::unexpected(rendered.error());
    const auto inserted = widths.emplace(safe_width, std::move(*rendered)).first;
    return std::cref(inserted->second);
}

support::Expected<std::size_t> LayoutBuilder::measure_height(
        LayoutContext& context, Component& component, std::size_t width) {
    auto rendered = render_cached(context, component, width);
    if (!rendered) return std::unexpected(rendered.error());
    return rendered->get().lines.size();
}

support::Expected<std::size_t> LayoutBuilder::measure_width(
        LayoutContext& context, Component& component, std::size_t width) {
    auto rendered = render_cached(context, component, width);
    if (!rendered) return std::unexpected(rendered.error());
    std::size_t measured = 0;
    for (const auto& line : rendered->get().lines)
        measured = std::max(measured, measure_visible_width(line).width);
    return measured;
}

std::vector<StackEntry> LayoutBuilder::stack_entry_views(const VStack& stack) {
    std::vector<StackEntry> entries;
    entries.reserve(stack.entries_.size());
    for (const auto& entry : stack.entries_)
        entries.push_back(StackEntry{.component = entry.component.get(), .options = &entry.options});
    return entries;
}

std::vector<StackEntry> LayoutBuilder::stack_entry_views(const HStack& stack) {
    std::vector<StackEntry> entries;
    entries.reserve(stack.entries_.size());
    for (const auto& entry : stack.entries_)
        entries.push_back(StackEntry{.component = entry.component.get(), .options = &entry.options});
    return entries;
}

support::Expected<LayoutBox> LayoutBuilder::layout_component(LayoutContext& context,
        Component& component,
        std::ptrdiff_t x,
        std::ptrdiff_t y,
        std::size_t width,
        std::optional<std::size_t> height,
        const LayoutRect& clip) {
    const auto safe_width = std::max<std::size_t>(1, width);
    if (auto* scroll = dynamic_cast<ScrollView*>(&component); scroll != nullptr) {
        // Scroll node (pi `layout.ts` scroll branch): the child is laid out
        // against the viewport's incoming clip at the previous scroll position
        // with no height bound, so its whole content is measured. An `always`
        // scrollbar reserves its last column before the child wraps, so the
        // reservation changes the content height the viewport then learns. The
        // viewport then learns the heights it allocated, and the child is
        // shifted by the distance the settled scroll position moved it.
        const auto previous_scroll_top = scroll->scroll_top();
        const auto content_width = scroll->content_width(safe_width);
        auto child = layout_component(context,
                *scroll->child_,
                x,
                y - static_cast<std::ptrdiff_t>(previous_scroll_top),
                content_width,
                std::nullopt,
                clip);
        if (!child) return std::unexpected(child.error());
        const auto content_height = child->rect.height;
        const auto viewport_height = height ? *height : content_height;
        auto scroll_content = render_cached(context, *scroll->child_, content_width);
        if (!scroll_content) return std::unexpected(scroll_content.error());
        scroll->update_layout(content_height, viewport_height, context.request_render);
        translate_box(*child,
                static_cast<std::ptrdiff_t>(previous_scroll_top) - static_cast<std::ptrdiff_t>(scroll->scroll_top()));
        if (scroll->primary_ || context.primary_scroll_view == nullptr) context.primary_scroll_view = scroll;
        const auto rect = LayoutRect{.x = x, .y = y, .width = safe_width, .height = viewport_height};
        const auto child_clip = intersect(clip, rect);
        LayoutBox box{.component = &component,
                .rect = rect,
                .clip = child_clip,
                .children = {},
                .leaf = false,
                .line_offset = 0,
                .scroll_view = scroll,
                .scroll_content_lines = scroll_content->get().lines,
                .has_scroll_content_lines = true};
        box.children.push_back(std::move(*child));
        update_clips(box.children.front(), child_clip);
        return box;
    }

    const auto* vstack = dynamic_cast<const VStack*>(&component);
    const auto* hstack = dynamic_cast<const HStack*>(&component);
    if (vstack == nullptr && hstack == nullptr) {
        // Leaf: a component without a layout node owns the lines it renders
        // (pi `layout.ts` leaf branch).
        auto rendered = render_cached(context, component, safe_width);
        if (!rendered) return std::unexpected(rendered.error());
        const auto& lines = rendered->get().lines;
        const auto allocated_height = height ? *height : lines.size();
        std::size_t line_offset = 0;
        if (lines.size() > allocated_height && allocated_height > 0) {
            if (const auto cursor = component.cursor_location(); cursor && cursor->row >= allocated_height) {
                line_offset = cursor->row - allocated_height + 1;
            }
        }
        const auto rect = LayoutRect{.x = x, .y = y, .width = safe_width, .height = allocated_height};
        return LayoutBox{.component = &component,
                .rect = rect,
                .clip = intersect(clip, rect),
                .children = {},
                .leaf = true,
                .line_offset = line_offset};
    }

    if (hstack != nullptr) {
        // Horizontal stack node (pi `layout.ts` hstack branch): the frame width
        // is the main axis, so widths are allocated against it, every child's
        // height is measured at its own allocated width, and `align` places the
        // child on the cross axis.
        const auto entries = stack_entry_views(*hstack);
        const auto visible = visible_stack_entries(entries, context.viewport);

        std::vector<std::size_t> intrinsic_widths;
        intrinsic_widths.reserve(visible.size());
        for (const auto& entry : visible) {
            if (entry.options->basis) {
                intrinsic_widths.push_back(*entry.options->basis);
                continue;
            }
            auto measured = measure_width(context, *entry.component, safe_width);
            if (!measured) return std::unexpected(measured.error());
            intrinsic_widths.push_back(*measured);
        }
        const auto widths = allocate_stack_sizes(visible, intrinsic_widths, safe_width, hstack->gap_);

        std::vector<std::size_t> intrinsic_heights;
        intrinsic_heights.reserve(visible.size());
        for (std::size_t index = 0; index < visible.size(); ++index) {
            auto measured = measure_height(context, *visible[index].component, std::max<std::size_t>(1, widths[index]));
            if (!measured) return std::unexpected(measured.error());
            intrinsic_heights.push_back(*measured);
        }
        std::size_t tallest = 0;
        for (const auto child_height : intrinsic_heights)
            tallest = std::max(tallest, child_height);
        const auto rect = LayoutRect{.x = x, .y = y, .width = safe_width, .height = height ? *height : tallest};
        LayoutBox box{.component = &component,
                .rect = rect,
                .clip = intersect(clip, rect),
                .children = {},
                .leaf = false,
                .line_offset = 0};
        box.children.reserve(visible.size());
        auto child_x = x;
        for (std::size_t index = 0; index < visible.size(); ++index) {
            const auto natural_height = intrinsic_heights[index];
            const auto child_height =
                    hstack->align_ == StackAlign::Stretch ? rect.height : std::min(rect.height, natural_height);
            auto child_y = y;
            if (hstack->align_ == StackAlign::Center)
                child_y += static_cast<std::ptrdiff_t>((rect.height - child_height) / 2);
            else if (hstack->align_ == StackAlign::End)
                child_y += static_cast<std::ptrdiff_t>(rect.height - child_height);

            const auto child_width = widths[index];
            if (child_width == 0) {
                // A zero-width entry is laid out as an empty box without
                // rendering its component, exactly as pi skips it.
                box.children.push_back(LayoutBox{.component = visible[index].component,
                        .rect = LayoutRect{.x = child_x, .y = child_y, .width = 0, .height = child_height},
                        .clip = LayoutRect{.x = child_x, .y = child_y, .width = 0, .height = 0},
                        .children = {},
                        .leaf = false,
                        .line_offset = 0});
            } else {
                auto child = layout_component(
                        context, *visible[index].component, child_x, child_y, child_width, child_height, box.clip);
                if (!child) return std::unexpected(child.error());
                box.children.push_back(std::move(*child));
            }
            child_x += static_cast<std::ptrdiff_t>(child_width + hstack->gap_);
        }
        return box;
    }

    // Vertical stack node (pi `layout.ts` vstack branch): the frame width is the
    // cross axis, `allocate_stack_sizes` gives every visible entry its share of
    // the main axis, and children are placed in entry order.
    const auto entries = stack_entry_views(*vstack);
    const auto visible = visible_stack_entries(entries, context.viewport);
    const auto gap_total = visible.empty() ? std::size_t{0} : (visible.size() - 1) * vstack->gap_;

    std::vector<std::size_t> intrinsic_heights;
    intrinsic_heights.reserve(visible.size());
    for (const auto& entry : visible) {
        if (entry.options->basis) {
            intrinsic_heights.push_back(*entry.options->basis);
            continue;
        }
        auto measured = measure_height(context, *entry.component, safe_width);
        if (!measured) return std::unexpected(measured.error());
        intrinsic_heights.push_back(*measured);
    }
    const auto sizes = allocate_stack_sizes(visible, intrinsic_heights, height, vstack->gap_);
    auto natural_height = gap_total;
    for (const auto size : sizes)
        natural_height += size;
    const auto rect = LayoutRect{.x = x, .y = y, .width = safe_width, .height = height ? *height : natural_height};
    LayoutBox box{.component = &component,
            .rect = rect,
            .clip = intersect(clip, rect),
            .children = {},
            .leaf = false,
            .line_offset = 0};
    box.children.reserve(visible.size());
    auto child_y = y;
    for (std::size_t index = 0; index < visible.size(); ++index) {
        auto child =
                layout_component(context, *visible[index].component, x, child_y, safe_width, sizes[index], box.clip);
        if (!child) return std::unexpected(child.error());
        box.children.push_back(std::move(*child));
        child_y += static_cast<std::ptrdiff_t>(sizes[index] + vstack->gap_);
    }
    return box;
}

support::ExpectedVoid LayoutBuilder::paint_box(LayoutContext& context, const LayoutBox& box, LayoutFrame& frame) {
    const auto screen_height = static_cast<std::ptrdiff_t>(frame.lines.size());
    if (box.leaf) {
        auto rendered = render_cached(context, *box.component, box.rect.width);
        if (!rendered) return std::unexpected(rendered.error());
        const auto& lines = rendered->get().lines;
        // pi `layout.ts` `paintBox`: rows come from the box's rect and clip, the
        // source row is shifted by the cursor crop, and the row is written
        // directly only when the box owns the whole frame width and no earlier
        // box painted it. Any other box covers part of a row a sibling may
        // already hold, so its columns are spliced into the composed row
        // (pi `compositeTuiLine`).
        const auto first_row = std::max<std::ptrdiff_t>({box.rect.y, box.clip.y, 0});
        const auto last_row = std::min<std::ptrdiff_t>({box.rect.y + static_cast<std::ptrdiff_t>(box.rect.height),
                box.clip.y + static_cast<std::ptrdiff_t>(box.clip.height),
                screen_height});
        for (auto row = first_row; row < last_row; ++row) {
            const auto source_index = static_cast<std::size_t>(row - box.rect.y) + box.line_offset;
            if (source_index >= lines.size()) continue;
            // The leaf rendered at its allocated width, so that width is the
            // bound its own lines must respect.
            auto prepared = prepare_rendered_line(lines[source_index], box.rect.width);
            if (!prepared) return std::unexpected(prepared.error());
            const auto target = static_cast<std::size_t>(row);
            const auto owns_row = box.rect.x == 0 && box.rect.width >= context.viewport.width &&
                                  prepared->width <= context.viewport.width;
            if (owns_row && (is_image_line(prepared->text) || frame.lines[target].empty())) {
                frame.lines[target] = std::move(prepared->text);
                continue;
            }
            auto composed = detail::replace_line_region(frame.lines[target],
                    prepared->text,
                    static_cast<std::size_t>(box.rect.x),
                    box.rect.width,
                    context.viewport.width);
            if (!composed) return std::unexpected(composed.error());
            frame.lines[target] = std::move(*composed);
        }
        if (!frame.cursor) {
            // The first leaf in paint order whose painted rows carry a cursor row
            // owns the frame cursor, and the row must be a painted one: pi
            // `extractCursorPosition` (`tui.ts:1442`) can only find the marker a
            // written line carries. Column clamping stays with the presenter
            // (pi `tui-alt-screen.ts:1765` places the cursor at `min(width, col)`).
            if (const auto location = box.component->cursor_location()) {
                const auto row = box.rect.y + static_cast<std::ptrdiff_t>(location->row) -
                                 static_cast<std::ptrdiff_t>(box.line_offset);
                const auto column = box.rect.x + static_cast<std::ptrdiff_t>(location->column);
                if (row >= first_row && row < last_row) {
                    frame.cursor = CursorPosition{
                            .column = static_cast<std::size_t>(column), .row = static_cast<std::size_t>(row)};
                }
            }
        }
        // Image placements are translated like the lines they reserve. Rows
        // the painted range cannot show are clipped away from the placement's
        // destination and recorded as source rows the consumer must crop: a
        // scrolled image keeps its visible part at the window edge, and a
        // placement with no visible row is absent from the frame. The
        // destination column rule and the protocol crop are #1012's.
        for (auto image : rendered->get().images) {
            const auto row = box.rect.y + static_cast<std::ptrdiff_t>(image.region.row) -
                             static_cast<std::ptrdiff_t>(box.line_offset);
            const auto column = box.rect.x + static_cast<std::ptrdiff_t>(image.region.column);
            const auto rows = static_cast<std::ptrdiff_t>(image.region.rows);
            const auto columns = static_cast<std::ptrdiff_t>(image.region.columns);
            if (column < box.clip.x || column + columns > static_cast<std::ptrdiff_t>(context.viewport.width)) continue;
            const auto first_visible_row = std::max(row, first_row);
            const auto last_visible_row = std::min(row + rows, last_row);
            if (last_visible_row <= first_visible_row) continue;
            image.source_row_offset += static_cast<std::size_t>(first_visible_row - row);
            image.region.row = static_cast<std::size_t>(first_visible_row);
            image.region.rows = static_cast<std::size_t>(last_visible_row - first_visible_row);
            frame.images.push_back(std::move(image));
        }
    }
    for (const auto& child : box.children) {
        if (auto painted = paint_box(context, child, frame); !painted) return painted;
    }
    // The scrollbar is the viewport's own last paint (pi `paintBox`), so a
    // floating bar overwrites the content column it shares.
    if (box.scroll_view != nullptr) {
        if (auto painted = paint_scrollbar(box, frame); !painted) return painted;
    }
    return {};
}

support::Expected<LayoutFrame> LayoutBuilder::build(
        Component& root, std::size_t width, std::size_t height, std::function<void()> request_render) {
    const auto safe_width = std::max<std::size_t>(1, width);
    const auto safe_height = std::max<std::size_t>(1, height);
    LayoutContext context{.viewport = LayoutViewport{.width = safe_width, .height = safe_height},
            .render_cache = {},
            .request_render = std::move(request_render),
            .primary_scroll_view = nullptr};
    const auto bounds = LayoutRect{.x = 0, .y = 0, .width = safe_width, .height = safe_height};
    auto root_box = layout_component(context, root, 0, 0, safe_width, safe_height, bounds);
    if (!root_box) return std::unexpected(root_box.error());

    LayoutFrame frame{.root = std::move(*root_box),
            .width = safe_width,
            .height = safe_height,
            .lines = std::vector<std::string>(safe_height),
            .images = {},
            .cursor = std::nullopt,
            .primary_scroll_view = context.primary_scroll_view};
    if (auto painted = paint_box(context, frame.root, frame); !painted) return std::unexpected(painted.error());
    return frame;
}

support::Expected<LayoutFrame> LayoutEngine::render(
        Component& root, std::size_t width, std::size_t height, std::function<void()> request_render) {
    return LayoutBuilder::build(root, width, height, std::move(request_render));
}

std::vector<const LayoutBox*> layout_boxes_at(const LayoutFrame& frame, std::ptrdiff_t x, std::ptrdiff_t y) {
    std::vector<std::pair<const LayoutBox*, std::size_t>> found;
    collect_boxes_at(frame.root, x, y, 0, found);
    // Every box of the frozen frame carries `layer: 0` (pi `layout.ts:132-234`),
    // so the visual hit path is the depth-descending order of pi
    // `getLayoutBoxesAt`.
    std::stable_sort(
            found.begin(), found.end(), [](const auto& left, const auto& right) { return left.second > right.second; });
    std::vector<const LayoutBox*> boxes;
    boxes.reserve(found.size());
    for (const auto& entry : found)
        boxes.push_back(entry.first);
    return boxes;
}

std::vector<ScrollView*> scroll_views_at(const LayoutFrame& frame, std::ptrdiff_t x, std::ptrdiff_t y) {
    std::vector<std::pair<ScrollView*, std::size_t>> found;
    collect_scroll_views(frame.root, x, y, 0, found);
    std::stable_sort(
            found.begin(), found.end(), [](const auto& left, const auto& right) { return left.second > right.second; });
    std::vector<ScrollView*> viewports;
    viewports.reserve(found.size());
    for (const auto& entry : found)
        viewports.push_back(entry.first);
    return viewports;
}

std::optional<ScrollbarGeometry> get_scrollbar_geometry(const LayoutBox& box, bool include_hidden_auto) {
    if (box.scroll_view == nullptr || box.rect.width == 0 || box.rect.height == 0) return std::nullopt;

    const auto content_height = box.children.empty() ? std::size_t{0} : box.children.front().rect.height;
    const auto track_height = box.rect.height;
    const auto can_reveal_hidden_auto =
            include_hidden_auto && box.scroll_view->scrollbar() == ScrollbarMode::Auto && content_height > track_height;
    if (!box.scroll_view->is_scrollbar_visible() && !can_reveal_hidden_auto) return std::nullopt;

    const auto min_thumb_height = std::min<std::size_t>(2, track_height);
    // pi rounds `trackHeight * trackHeight / contentHeight`; empty content is
    // its `Infinity` branch, where the thumb covers the whole track.
    const auto proportional = content_height == 0
                                      ? track_height
                                      : static_cast<std::size_t>(std::llround(static_cast<double>(track_height) *
                                                                              static_cast<double>(track_height) /
                                                                              static_cast<double>(content_height)));
    const auto thumb_height = std::max(min_thumb_height, std::min(track_height, proportional));
    const auto max_scroll_top = content_height > track_height ? content_height - track_height : std::size_t{0};
    const auto max_thumb_top = track_height - thumb_height;
    const auto thumb_offset =
            max_scroll_top == 0 ? std::size_t{0}
                                : static_cast<std::size_t>(std::llround(
                                          static_cast<double>(box.scroll_view->scroll_top()) /
                                          static_cast<double>(max_scroll_top) * static_cast<double>(max_thumb_top)));
    const auto column = box.rect.x + static_cast<std::ptrdiff_t>(box.rect.width) - 1;
    if (column < box.clip.x || column >= box.clip.x + static_cast<std::ptrdiff_t>(box.clip.width)) return std::nullopt;

    return ScrollbarGeometry{.column = column,
            .track_top = box.rect.y,
            .track_height = track_height,
            .thumb_top = box.rect.y + static_cast<std::ptrdiff_t>(thumb_offset),
            .thumb_height = thumb_height,
            .max_scroll_top = max_scroll_top};
}

const LayoutBox* get_scroll_view_box(const LayoutFrame& frame, const ScrollView* viewport) {
    return find_scroll_box(frame.root, viewport);
}

} // namespace cch::tui::detail

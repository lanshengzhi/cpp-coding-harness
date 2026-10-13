#include <cch/tui/HStack.hpp>

#include "tui/LineCompositor.hpp"
#include "tui/StackLayout.hpp"
#include "tui/UnicodeWidth.hpp"

#include <cch/support/Error.hpp>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace cch::tui {

HStack::HStack(StackOptions options) : gap_(options.gap), align_(options.align) {}

HStack::HStack(HStack&&) noexcept = default;
HStack& HStack::operator=(HStack&&) noexcept = default;
HStack::~HStack() = default;

support::Expected<std::reference_wrapper<Component>> HStack::add_child(
        std::unique_ptr<Component> component, StackEntryOptions options) {
    if (!component) {
        return std::unexpected(
                support::make_error(support::ErrorCode::Validation, "TUI HStack cannot attach a null Component"));
    }
    auto& child = *component;
    entries_.push_back(Entry{.component = std::move(component), .options = std::move(options)});
    rendered_placements_.clear();
    render_valid_ = false;
    return child;
}

std::unique_ptr<Component> HStack::remove_child(const Component* component) {
    const auto found = std::find_if(entries_.begin(), entries_.end(), [component](const Entry& entry) {
        return entry.component.get() == component;
    });
    if (found == entries_.end()) return nullptr;

    auto removed = std::move(found->component);
    entries_.erase(found);
    rendered_placements_.clear();
    render_valid_ = false;
    return removed;
}

void HStack::clear() {
    entries_.clear();
    rendered_placements_.clear();
    render_valid_ = false;
}

support::Expected<RenderResult> HStack::render(std::size_t width) {
    rendered_placements_.clear();
    render_valid_ = false;
    if (width == 0) {
        return std::unexpected(
                support::make_error(support::ErrorCode::Validation, "TUI HStack requires a positive visible width"));
    }

    // pi `h-stack.ts` `render`: every visible child renders once at the full
    // width to measure its intrinsic width, the widths are allocated against
    // the requested width minus `gap` columns, and every nonzero allocation
    // renders the child again at exactly that width before its lines are
    // composed into the shared rows at that column.
    const LayoutViewport viewport{.width = width, .height = kStackUnboundedSize};
    std::vector<std::size_t> visible_indices;
    visible_indices.reserve(entries_.size());
    for (std::size_t index = 0; index < entries_.size(); ++index) {
        const auto& options = entries_[index].options;
        if (options.visible && !options.visible(viewport)) continue;
        visible_indices.push_back(index);
    }
    std::vector<detail::StackEntry> views;
    views.reserve(visible_indices.size());
    for (const auto index : visible_indices) {
        views.push_back(
                detail::StackEntry{.component = entries_[index].component.get(), .options = &entries_[index].options});
    }

    std::vector<std::optional<RenderedEntryPlacement>> placements(entries_.size(), std::nullopt);
    if (visible_indices.empty()) {
        rendered_placements_ = std::move(placements);
        render_valid_ = true;
        return RenderResult{};
    }

    std::vector<std::size_t> intrinsic_widths;
    intrinsic_widths.reserve(visible_indices.size());
    for (const auto index : visible_indices) {
        auto measured = entries_[index].component->render(width);
        if (!measured) return std::unexpected(measured.error());
        std::size_t intrinsic = 0;
        for (const auto& line : measured->lines)
            intrinsic = std::max(intrinsic, detail::measure_visible_width(line).width);
        intrinsic_widths.push_back(intrinsic);
    }
    const auto widths = detail::allocate_stack_sizes(views, intrinsic_widths, width, gap_);

    std::vector<RenderResult> rendered;
    rendered.reserve(visible_indices.size());
    for (std::size_t visible_index = 0; visible_index < visible_indices.size(); ++visible_index) {
        if (widths[visible_index] == 0) {
            // A zero-width allocation is never rendered, exactly as pi skips
            // `render(widths[index])` for it.
            rendered.emplace_back();
            continue;
        }
        auto child = entries_[visible_indices[visible_index]].component->render(widths[visible_index]);
        if (!child) return std::unexpected(child.error());
        rendered.push_back(std::move(*child));
    }

    std::size_t height = 0;
    for (const auto& child : rendered)
        height = std::max(height, child.lines.size());

    RenderResult result;
    result.lines = std::vector<std::string>(height);
    std::size_t column = 0;
    for (std::size_t visible_index = 0; visible_index < visible_indices.size(); ++visible_index) {
        auto& child = rendered[visible_index];
        const auto child_width = widths[visible_index];
        std::size_t row_offset = 0;
        if (align_ == StackAlign::Center)
            row_offset = (height - child.lines.size()) / 2;
        else if (align_ == StackAlign::End)
            row_offset = height - child.lines.size();

        for (std::size_t row = 0; row < child.lines.size(); ++row) {
            const auto target = row + row_offset;
            if (target >= result.lines.size()) continue;
            // The row already holds the entries left of this one, so the child
            // covers exactly its own columns (pi `compositeTuiLine`).
            auto composed =
                    detail::replace_line_region(result.lines[target], child.lines[row], column, child_width, width);
            if (!composed) return std::unexpected(composed.error());
            result.lines[target] = std::move(*composed);
        }
        for (auto& image : child.images) {
            // A placement that the allocation or the composed rows cannot hold
            // is dropped; the constrained frame owns placement decisions.
            if (image.region.column + image.region.columns > child_width) continue;
            const auto image_row = row_offset + image.region.row;
            if (image_row + image.region.rows > height) continue;
            image.region.row = image_row;
            image.region.column += column;
            result.images.push_back(std::move(image));
        }
        placements[visible_indices[visible_index]] = RenderedEntryPlacement{
                .column = column, .width = child_width, .row_offset = row_offset, .rows = child.lines.size()};
        column += child_width + gap_;
    }
    rendered_placements_ = std::move(placements);
    render_valid_ = true;
    return result;
}

void HStack::invalidate() {
    rendered_placements_.clear();
    render_valid_ = false;
    for (const auto& entry : entries_)
        entry.component->invalidate();
}

std::optional<CursorPosition> HStack::cursor_location() const {
    if (!render_valid_ || rendered_placements_.size() != entries_.size()) return std::nullopt;

    for (std::size_t index = 0; index < entries_.size(); ++index) {
        const auto& placement = rendered_placements_[index];
        if (!placement) continue;
        const auto location = entries_[index].component->cursor_location();
        if (!location) continue;
        // A cursor row that the allocation or the composed result cropped away
        // cannot be the frame cursor, exactly as a cropped cursor marker
        // disappears. The column is the child's own column inside the region
        // the entry was composed into.
        if (location->row >= placement->rows) continue;
        return CursorPosition{
                .column = placement->column + location->column, .row = placement->row_offset + location->row};
    }
    return std::nullopt;
}

bool HStack::contains(const Component* target) const {
    if (this == target) return true;
    for (const auto& entry : entries_) {
        if (entry.component && entry.component->contains(target)) return true;
    }
    return false;
}

} // namespace cch::tui

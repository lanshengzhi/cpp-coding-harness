#include <cch/tui/VStack.hpp>

#include "tui/RenderUtils.hpp"
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

VStack::VStack(StackOptions options) : gap_(options.gap), align_(options.align) {}

VStack::VStack(VStack&&) noexcept = default;
VStack& VStack::operator=(VStack&&) noexcept = default;
VStack::~VStack() = default;

support::Expected<std::reference_wrapper<Component>> VStack::add_child(
        std::unique_ptr<Component> component, StackEntryOptions options) {
    if (!component) {
        return std::unexpected(
                support::make_error(support::ErrorCode::Validation, "TUI VStack cannot attach a null Component"));
    }
    auto& child = *component;
    entries_.push_back(Entry{.component = std::move(component), .options = std::move(options)});
    rendered_child_rows_.clear();
    render_valid_ = false;
    return child;
}

std::unique_ptr<Component> VStack::remove_child(const Component* component) {
    const auto found = std::find_if(entries_.begin(), entries_.end(), [component](const Entry& entry) {
        return entry.component.get() == component;
    });
    if (found == entries_.end()) return nullptr;

    auto removed = std::move(found->component);
    entries_.erase(found);
    rendered_child_rows_.clear();
    render_valid_ = false;
    return removed;
}

void VStack::clear() {
    entries_.clear();
    rendered_child_rows_.clear();
    render_valid_ = false;
}

support::Expected<RenderResult> VStack::render(std::size_t width) {
    rendered_child_rows_.clear();
    render_valid_ = false;
    if (width == 0) {
        return std::unexpected(
                support::make_error(support::ErrorCode::Validation, "TUI VStack requires a positive visible width"));
    }

    // pi `v-stack.ts` `render`: the standalone stack measures against an
    // unbounded height, so only `basis`/`minSize`/`maxSize` select each size
    // and the visible entries keep their order with `gap` rows between them.
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

    std::vector<RenderResult> rendered;
    std::vector<std::size_t> intrinsic_sizes;
    rendered.reserve(visible_indices.size());
    intrinsic_sizes.reserve(visible_indices.size());
    for (const auto index : visible_indices) {
        auto child = entries_[index].component->render(width);
        if (!child) return std::unexpected(child.error());
        intrinsic_sizes.push_back(child->lines.size());
        rendered.push_back(std::move(*child));
    }
    const auto sizes = detail::allocate_stack_sizes(views, intrinsic_sizes, std::nullopt, gap_);

    RenderResult result;
    std::vector<std::optional<std::size_t>> rendered_child_rows(entries_.size(), std::nullopt);
    for (std::size_t visible_index = 0; visible_index < visible_indices.size(); ++visible_index) {
        auto& child = rendered[visible_index];
        const auto size = sizes[visible_index];
        if (visible_index > 0) {
            for (std::size_t gap = 0; gap < gap_; ++gap)
                result.lines.emplace_back();
        }
        const auto row_offset = result.lines.size();
        std::size_t emitted = 0;
        for (auto& line : child.lines) {
            if (emitted == size) break;
            auto prepared = detail::prepare_rendered_line(line, width);
            if (!prepared) return std::unexpected(prepared.error());
            result.lines.push_back(std::move(prepared->text));
            ++emitted;
        }
        for (; emitted < size; ++emitted)
            result.lines.emplace_back();
        for (auto& image : child.images) {
            // A placement that the allocation or the width cannot hold is
            // dropped; the constrained frame owns placement decisions.
            if (image.region.row + image.region.rows > size) continue;
            if (image.region.column + image.region.columns > width) continue;
            image.region.row += row_offset;
            result.images.push_back(std::move(image));
        }
        rendered_child_rows[visible_indices[visible_index]] = size;
    }
    rendered_child_rows_ = std::move(rendered_child_rows);
    render_valid_ = true;
    return result;
}

void VStack::invalidate() {
    rendered_child_rows_.clear();
    render_valid_ = false;
    for (const auto& entry : entries_)
        entry.component->invalidate();
}

std::optional<CursorPosition> VStack::cursor_location() const {
    if (!render_valid_ || rendered_child_rows_.size() != entries_.size()) return std::nullopt;

    std::size_t last_visible = entries_.size();
    for (std::size_t index = entries_.size(); index-- > 0;) {
        if (rendered_child_rows_[index]) {
            last_visible = index;
            break;
        }
    }

    std::size_t row_offset = 0;
    for (std::size_t index = 0; index < entries_.size(); ++index) {
        const auto rows = rendered_child_rows_[index];
        if (!rows) continue;
        if (const auto location = entries_[index].component->cursor_location()) {
            // A cursor line that the allocation cropped away cannot be the
            // frame cursor, exactly as a cropped cursor marker disappears.
            if (location->row < *rows) {
                return CursorPosition{.column = location->column, .row = row_offset + location->row};
            }
        }
        row_offset += *rows;
        if (index != last_visible) row_offset += gap_;
    }
    return std::nullopt;
}

bool VStack::contains(const Component* target) const {
    if (this == target) return true;
    for (const auto& entry : entries_) {
        if (entry.component && entry.component->contains(target)) return true;
    }
    return false;
}

} // namespace cch::tui

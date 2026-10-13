#pragma once

#include <cch/tui/Stack.hpp>

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace cch::tui {

class Component;

namespace detail {

/// Passive view of one stack entry (pi `StackEntry`). The referenced component
/// and options must outlive the view; callers build views over a live stack.
struct StackEntry {
    Component* component{nullptr};
    const StackEntryOptions* options{nullptr};
};

/// The entries whose `visible` predicate accepts `viewport`, in stack order
/// (pi `visibleStackEntries`); the returned views keep referencing the same
/// options.
[[nodiscard]] std::vector<StackEntry> visible_stack_entries(
        std::span<const StackEntry> entries, const LayoutViewport& viewport);

/// Allocate one size per entry (pi `allocateStackSizes`). Each size starts at
/// its `basis` or intrinsic size clamped to `[min_size, max_size]`; with an
/// available size, the difference from `available_size` minus gaps is
/// distributed by `grow` weight (filling up, saturating at `max_size`) or by
/// `shrink * max(1, size)` weight (giving back, saturating at `min_size`).
[[nodiscard]] std::vector<std::size_t> allocate_stack_sizes(std::span<const StackEntry> entries,
        std::span<const std::size_t> intrinsic_sizes,
        std::optional<std::size_t> available_size,
        std::size_t gap);

} // namespace detail

} // namespace cch::tui

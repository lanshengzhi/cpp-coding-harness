#include "tui/StackLayout.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace cch::tui::detail {
namespace {

enum class DistributeMode { Grow, Shrink };

/// pi `stack.ts` `clampSize`: a requested size lands inside
/// `[minSize, max(minSize, maxSize)]`.
[[nodiscard]] std::size_t clamp_size(std::size_t size, const StackEntryOptions& options) {
    return std::clamp(size, options.min_size, std::max(options.min_size, options.max_size));
}

/// pi `stack.ts` `distribute` weight: `grow`, or `shrink * max(1, size)` so a
/// larger allocation gives back proportionally more.
[[nodiscard]] std::size_t distribute_weight(const StackEntryOptions& options, DistributeMode mode, std::size_t size) {
    if (mode == DistributeMode::Grow) return options.grow;
    return options.shrink * std::max<std::size_t>(1, size);
}

/// pi `stack.ts` `distribute` candidate test, which reads the raw `maxSize`
/// (not the clamped bound).
[[nodiscard]] bool is_candidate(const StackEntryOptions& options, DistributeMode mode, std::size_t size) {
    if (mode == DistributeMode::Grow) return options.grow > 0 && size < options.max_size;
    return options.shrink > 0 && size > options.min_size;
}

/// `floor(remaining * weight / totalWeight)` as exact integer arithmetic, so a
/// large viewport cannot overflow the product.
[[nodiscard]] std::size_t proposed_share(std::size_t remaining, std::size_t weight, std::size_t total_weight) {
    const auto quotient = remaining / total_weight;
    const auto remainder = remaining % total_weight;
    return std::max<std::size_t>(1, quotient * weight + (remainder * weight) / total_weight);
}

/// pi `stack.ts` `distribute`: repeatedly hand out (grow) or take back (shrink)
/// cells, weighted per candidate, until `amount` is spent or nothing can move.
void distribute(
        std::vector<std::size_t>& sizes, std::span<const StackEntry> entries, std::size_t amount, DistributeMode mode) {
    auto remaining = amount;
    while (remaining > 0) {
        std::size_t total_weight = 0;
        for (std::size_t index = 0; index < entries.size(); ++index) {
            const auto& options = *entries[index].options;
            if (!is_candidate(options, mode, sizes[index])) continue;
            total_weight += distribute_weight(options, mode, sizes[index]);
        }
        if (total_weight == 0) return;

        std::size_t distributed = 0;
        for (std::size_t index = 0; index < entries.size(); ++index) {
            if (remaining == 0) break;
            const auto& options = *entries[index].options;
            if (!is_candidate(options, mode, sizes[index])) continue;
            const auto weight = distribute_weight(options, mode, sizes[index]);
            const auto proposed = proposed_share(remaining, weight, total_weight);
            const auto capacity =
                    mode == DistributeMode::Grow ? options.max_size - sizes[index] : sizes[index] - options.min_size;
            const auto delta = std::min({remaining, proposed, capacity});
            if (delta == 0) continue;
            sizes[index] = mode == DistributeMode::Grow ? sizes[index] + delta : sizes[index] - delta;
            remaining -= delta;
            distributed += delta;
        }
        if (distributed == 0) return;
    }
}

} // namespace

std::vector<StackEntry> visible_stack_entries(std::span<const StackEntry> entries, const LayoutViewport& viewport) {
    std::vector<StackEntry> visible;
    visible.reserve(entries.size());
    for (const auto& entry : entries) {
        if (entry.options->visible && !entry.options->visible(viewport)) continue;
        visible.push_back(entry);
    }
    return visible;
}

std::vector<std::size_t> allocate_stack_sizes(std::span<const StackEntry> entries,
        std::span<const std::size_t> intrinsic_sizes,
        std::optional<std::size_t> available_size,
        std::size_t gap) {
    std::vector<std::size_t> sizes;
    sizes.reserve(entries.size());
    for (std::size_t index = 0; index < entries.size(); ++index) {
        const auto& options = *entries[index].options;
        const auto requested = options.basis
                                       ? *options.basis
                                       : (index < intrinsic_sizes.size() ? intrinsic_sizes[index] : std::size_t{0});
        sizes.push_back(clamp_size(requested, options));
    }
    if (!available_size) return sizes;

    const auto gap_total = entries.empty() ? std::size_t{0} : (entries.size() - 1) * gap;
    const auto content_size = *available_size > gap_total ? *available_size - gap_total : std::size_t{0};
    std::size_t total = 0;
    for (const auto size : sizes)
        total += size;
    if (total < content_size) {
        distribute(sizes, entries, content_size - total, DistributeMode::Grow);
    } else if (total > content_size) {
        distribute(sizes, entries, total - content_size, DistributeMode::Shrink);
    }
    return sizes;
}

} // namespace cch::tui::detail

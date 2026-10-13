#pragma once

#include <cstddef>
#include <functional>
#include <optional>

namespace cch::tui {

/// The viewport a stack offers to its entries while a frame is allocated
/// (pi `LayoutViewport`).
struct LayoutViewport {
    std::size_t width{1};
    std::size_t height{1};
};

/// pi's `Number.MAX_SAFE_INTEGER`: the default entry `max_size` and the
/// unbounded viewport height `VStack.render` allocates against.
inline constexpr std::size_t kStackUnboundedSize{9007199254740991};

/// pi `StackOptions.align`. A vertical stack carries it but never reads it: the
/// cross axis of a VStack is horizontal, and frozen `v-stack.ts` only allocates
/// its main axis. `HStack` (#999) is where alignment takes effect.
enum class StackAlign { Stretch, Start, Center, End };

/// One stack entry's layout options (pi `StackEntryOptions`). Sizes are visible
/// cells; an absent `basis` is pi's `"auto"` (the component's measured size).
struct StackEntryOptions {
    std::optional<std::size_t> basis{};
    std::size_t grow{0};
    std::size_t shrink{1};
    std::size_t min_size{0};
    std::size_t max_size{kStackUnboundedSize};
    /// Evaluated once per allocation with that layout's viewport, so a stack
    /// can drop entries that do not fit (pi `visibleStackEntries`).
    std::function<bool(const LayoutViewport&)> visible{};
};

/// Stack constructor options (pi `StackOptions`).
struct StackOptions {
    std::size_t gap{0};
    StackAlign align{StackAlign::Stretch};
};

} // namespace cch::tui

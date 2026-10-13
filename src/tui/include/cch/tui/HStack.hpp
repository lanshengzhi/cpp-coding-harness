#pragma once

#include <cch/tui/Component.hpp>
#include <cch/tui/Stack.hpp>

#include <cch/support/Error.hpp>

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace cch::tui {

namespace detail {
/// The engine's allocation and painting pass (defined in `tui/Layout.cpp`); it
/// reads this stack's node data (pi `LAYOUT_NODE`).
class LayoutBuilder;
} // namespace detail

/// A horizontal stack of child components (pi `HStack`, `h-stack.ts`).
///
/// Every child carries its own `StackEntryOptions`. Rendered on its own
/// (`render(width)`), the stack is bounded by the width it is given: each entry
/// is allocated its `basis` or measured width clamped to `[min_size, max_size]`,
/// `grow`/`shrink` distribute the difference from the requested width minus
/// `gap` columns, every nonzero-width child renders at its allocated width, and
/// its lines are composed into the shared rows at that column. Inside a
/// constrained frame the same options allocate against the frame width (pi
/// `layout.ts` hstack node), which is what places side-by-side regions, and
/// `align` positions each child on the cross axis.
class HStack final : public Component {
public:
    explicit HStack(StackOptions options = {});
    HStack(HStack&&) noexcept;
    HStack& operator=(HStack&&) noexcept;
    ~HStack() override;

    HStack(const HStack&) = delete;
    HStack& operator=(const HStack&) = delete;

    [[nodiscard]] support::Expected<std::reference_wrapper<Component>> add_child(
            std::unique_ptr<Component> component, StackEntryOptions options = {});
    /// Detach a child by identity and return its ownership, or null when absent.
    [[nodiscard]] std::unique_ptr<Component> remove_child(const Component* component);
    /// Remove all children and their entry options (pi `Stack.clear`).
    void clear();

    [[nodiscard]] support::Expected<RenderResult> render(std::size_t width) override;
    void invalidate() override;
    [[nodiscard]] std::optional<CursorPosition> cursor_location() const override;
    [[nodiscard]] bool contains(const Component* target) const override;

private:
    friend class detail::LayoutBuilder;

    struct Entry {
        std::unique_ptr<Component> component;
        StackEntryOptions options;
    };

    /// Where the last standalone render placed one entry, aligned with
    /// `entries_`; `nullopt` means the entry was not visible or the stack has
    /// not rendered. `row_offset` is the alignment offset of the entry's first
    /// row inside the composed result.
    struct RenderedEntryPlacement {
        std::size_t column{0};
        std::size_t width{0};
        std::size_t row_offset{0};
        std::size_t rows{0};
    };

    std::vector<Entry> entries_;
    std::size_t gap_{0};
    StackAlign align_{StackAlign::Stretch};
    std::vector<std::optional<RenderedEntryPlacement>> rendered_placements_;
    bool render_valid_{false};
};

} // namespace cch::tui

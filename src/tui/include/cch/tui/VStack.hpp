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
class LayoutEngine;
/// The engine's allocation and painting pass (defined in `tui/Layout.cpp`); it
/// reads this stack's node data (pi `LAYOUT_NODE`).
class LayoutBuilder;
} // namespace detail

/// A vertical stack of child components (pi `VStack`).
///
/// Every child carries its own `StackEntryOptions`. Rendered on its own
/// (`render(width)`), the stack is unbounded in height: each entry is allocated
/// its `basis` or measured size clamped to `[min_size, max_size]`, `grow` and
/// `shrink` do not apply, `gap` empty rows separate allocated entries, and
/// child output is cropped to the allocated size and padded with empty rows.
/// Inside a constrained frame the same options allocate against the frame
/// height (pi `layout.ts`), which is what fixes editor and footer regions at
/// the bottom of a fullscreen viewport.
class VStack final : public Component {
public:
    explicit VStack(StackOptions options = {});
    VStack(VStack&&) noexcept;
    VStack& operator=(VStack&&) noexcept;
    ~VStack() override;

    VStack(const VStack&) = delete;
    VStack& operator=(const VStack&) = delete;

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

    std::vector<Entry> entries_;
    std::size_t gap_{0};
    /// Carried for the frozen constructor contract only; see `Stack.hpp`.
    StackAlign align_{StackAlign::Stretch};
    /// Allocated rows per entry, aligned with `entries_`; an entry that was not
    /// visible holds no allocation. `cursor_location` offsets the reporting
    /// child by the preceding allocations and gaps.
    std::vector<std::optional<std::size_t>> rendered_child_rows_;
    bool render_valid_{false};
};

} // namespace cch::tui

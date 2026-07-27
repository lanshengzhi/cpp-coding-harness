#pragma once

#include <cch/tui/Component.hpp>

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace cch::tui {

/// Strategy for positioning an overlay relative to its anchor area.
enum class OverlayPosition {
    /// Top-left corner of overlay aligns with top-left of anchor.
    TopLeft,
    /// Top-right corner of overlay aligns with top-right of anchor.
    TopRight,
    /// Bottom-left corner of overlay aligns with bottom-left of anchor.
    BottomLeft,
    /// Bottom-right corner of overlay aligns with bottom-right of anchor.
    BottomRight,
    /// Centered horizontally and vertically within the anchor.
    Center,
    /// Anchored to the top edge, centered horizontally.
    TopCenter,
    /// Anchored to the bottom edge, centered horizontally.
    BottomCenter,
    /// Anchored to the left edge, centered vertically.
    LeftCenter,
    /// Anchored to the right edge, centered vertically.
    RightCenter,
    /// Absolute column/row position relative to the viewport.
    Absolute,
    /// Percentage of viewport for both column and row.
    Percentage,
};

/// Constraints for overlay sizing.
struct OverlaySizeConstraints {
    std::optional<std::size_t> min_width;
    std::optional<std::size_t> max_width;
    std::optional<std::size_t> min_height;
    std::optional<std::size_t> max_height;
};

/// Margins around an overlay.
struct OverlayMargins {
    std::size_t left{0};
    std::size_t right{0};
    std::size_t top{0};
    std::size_t bottom{0};
};

/// Visibility condition for responsive overlays.
struct OverlayVisibility {
    /// If set, the overlay is only visible when the viewport width
    /// is at least this value.
    std::optional<std::size_t> min_viewport_width;
    /// If set, the overlay is only visible when the viewport height
    /// is at least this value.
    std::optional<std::size_t> min_viewport_height;
    /// If set, the overlay is hidden when the viewport width exceeds
    /// this value.
    std::optional<std::size_t> max_viewport_width;
    /// If set, the overlay is hidden when the viewport height exceeds
    /// this value.
    std::optional<std::size_t> max_viewport_height;
};

/// Overlay-specific options.
struct OverlayOptions {
    OverlayPosition position{OverlayPosition::TopLeft};
    /// For Absolute position: column offset.
    std::size_t absolute_column{0};
    /// For Absolute position: row offset.
    std::size_t absolute_row{0};
    /// For Percentage position: column as a fraction of viewport width (0-100).
    std::size_t percentage_column{50};
    /// For Percentage position: row as a fraction of viewport height (0-100).
    std::size_t percentage_row{50};
    /// Size constraints for the overlay content area.
    OverlaySizeConstraints size_constraints;
    /// Margins around the overlay content inside its allocated area.
    OverlayMargins margins;
    /// Responsive visibility conditions.
    OverlayVisibility visibility;
    /// Stacking order (higher values appear on top).
    std::size_t z_index{0};
    /// If true, this overlay does not capture input (passes through to
    /// components beneath).
    bool non_capturing{false};
};

/// A positioned overlay that renders on top of base content.
///
/// Overlays support:
/// - Multiple position strategies (anchor-based, absolute, percentage)
/// - Size constraints, margins, and responsive visibility
/// - Z-index based stacking
/// - Non-capturing mode for transparent input pass-through
/// - Focus delegation to contained children
/// - Deterministic clipping to viewport bounds
class Overlay final : public Component, public InputHandler, public Focusable {
public:
    explicit Overlay(OverlayOptions options = {});
    Overlay(Overlay&&) noexcept;
    Overlay& operator=(Overlay&&) noexcept;
    ~Overlay() override;

    Overlay(const Overlay&) = delete;
    Overlay& operator=(const Overlay&) = delete;

    /// Add a child component to the overlay. The overlay may hold
    /// multiple children stacked in z-order.
    [[nodiscard]] util::Expected<std::reference_wrapper<Component>> add_child(
        std::unique_ptr<Component> component);

    /// Update the overlay options.
    void set_options(OverlayOptions options);
    [[nodiscard]] const OverlayOptions& options() const;

    /// Show or hide the overlay.
    void set_visible(bool visible);
    [[nodiscard]] bool visible() const;

    /// Set the anchor area within which position is computed.
    /// The anchor is defined by its top-left column/row, width and height.
    void set_anchor(std::size_t column, std::size_t row, std::size_t width, std::size_t height);

    /// Focus the first focusable child.
    [[nodiscard]] util::ExpectedVoid focus_first();

    /// Render the overlay content (without position offset).
    /// The caller (Tui) uses layout information to position output.
    [[nodiscard]] util::Expected<std::vector<std::string>> render(std::size_t width) override;
    void invalidate() override;

    // InputHandler (forwards to focused child)
    void handle_input(const InputEventVariant& input) override;
    [[nodiscard]] bool accepts_key_releases() const override;

    // Focusable
    void set_focused(bool focused) override;
    [[nodiscard]] bool focused() const override;
    [[nodiscard]] std::optional<CursorPosition> cursor_location() const override;

    /// Compute the absolute viewport position of the overlay given
    /// the viewport dimensions. Returns the top-left column/row where
    /// rendered output starts.
    [[nodiscard]] std::pair<std::size_t, std::size_t> layout_position(
        std::size_t viewport_width,
        std::size_t viewport_height,
        std::size_t content_width,
        std::size_t content_height) const;

    /// Returns true when this overlay would be visible given the viewport
    /// dimensions and visibility conditions.
    [[nodiscard]] bool visible_at(TerminalDimensions viewport) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace cch::tui

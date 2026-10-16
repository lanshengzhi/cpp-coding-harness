#pragma once

#include <cch/tui/Component.hpp>
#include <cch/tui/Mouse.hpp>

#include <cch/support/Error.hpp>

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cch::tui {

/// An absolute size or a percentage of the relevant terminal dimension.
struct SizeValue {
    double value{0.0};
    bool percentage{false};
    bool valid{true};

    constexpr SizeValue() = default;
    constexpr SizeValue(double absolute_value) : value(absolute_value) {}
    constexpr SizeValue(double size_value, bool is_percentage, bool is_valid)
        : value(size_value), percentage(is_percentage), valid(is_valid) {}
    SizeValue(const char* value_text);
    SizeValue(std::string_view value_text);
    SizeValue(const std::string& value_text);

    [[nodiscard]] static constexpr SizeValue absolute(double value) { return SizeValue(value, false, true); }

    [[nodiscard]] static constexpr SizeValue percent(double value) { return SizeValue(value, true, true); }
};

/// Anchor used when an explicit row or column is not supplied.
enum class OverlayAnchor {
    Center,
    TopLeft,
    TopRight,
    BottomLeft,
    BottomRight,
    TopCenter,
    BottomCenter,
    LeftCenter,
    RightCenter,
};

/// Margins between an overlay and the terminal viewport edges.
struct OverlayMargins {
    std::size_t left{0};
    std::size_t right{0};
    std::size_t top{0};
    std::size_t bottom{0};
};

/// Resolved overlay dimensions and terminal-relative position.
struct OverlayLayout {
    std::size_t width{1};
    std::size_t row{0};
    std::size_t col{0};
    std::optional<std::size_t> max_height{std::nullopt};
    std::size_t effective_height{0};
};

/// Overlay-specific options.
struct OverlayOptions {
    /// Width in columns, or a percentage of terminal width. Omitted width uses
    /// the smaller of 80 columns and the viewport remaining after margins.
    SizeValue width{80.0};
    std::size_t min_width{0};
    /// Maximum height in rows, or a percentage of terminal height.
    std::optional<SizeValue> max_height{std::nullopt};

    /// Anchor used independently for each unspecified coordinate.
    OverlayAnchor anchor{OverlayAnchor::Center};
    /// Absolute or percentage row and column positions.
    std::optional<SizeValue> row{std::nullopt};
    std::optional<SizeValue> col{std::nullopt};
    /// Signed displacement from the resolved position.
    std::ptrdiff_t offset_x{0};
    std::ptrdiff_t offset_y{0};

    OverlayMargins margins{};
    /// Optional responsive visibility rule evaluated for the current viewport.
    std::function<bool(TerminalDimensions)> visible_at{};

    /// Stacking order (higher values appear on top).
    std::size_t z_index{0};
    /// If true, this overlay does not capture input (passes through to
    /// components beneath).
    bool non_capturing{false};
};

/// A positioned overlay that renders on top of base content.
///
/// Overlays support percentage and absolute sizing, independent row and column
/// placement, viewport margins, responsive visibility, and deterministic
/// clipping to terminal bounds.
class Overlay final : public Component, public InputHandler, public Focusable, public MouseHandler {
public:
    explicit Overlay(OverlayOptions options = {});
    Overlay(Overlay&&) noexcept;
    Overlay& operator=(Overlay&&) noexcept;
    ~Overlay() override;

    Overlay(const Overlay&) = delete;
    Overlay& operator=(const Overlay&) = delete;

    /// Add a child component to the overlay. The overlay may hold
    /// multiple children stacked in z-order.
    [[nodiscard]] support::Expected<std::reference_wrapper<Component>> add_child(
        std::unique_ptr<Component> component);

    /// Update the overlay options.
    void set_options(OverlayOptions options);
    [[nodiscard]] const OverlayOptions& options() const;

    /// Show or hide the overlay.
    void set_visible(bool visible);
    [[nodiscard]] bool visible() const;

    /// Focus the first focusable child.
    [[nodiscard]] support::ExpectedVoid focus_first();

    /// Render the overlay content (without position offset).
    /// The caller (Tui) uses layout information to position output.
    [[nodiscard]] support::Expected<RenderResult> render(std::size_t width) override;
    void invalidate() override;

    // InputHandler (forwards to focused child)
    InputAdmissionOutcome handle_input(const InputEventVariant& input) override;

    // MouseHandler (forwards to the child keys reach)
    [[nodiscard]] std::optional<MouseDispatchResult> handle_mouse_forwarded(const MouseEvent& event) override;

    // Focusable
    void set_focused(bool focused) override;
    [[nodiscard]] bool focused() const override;
    [[nodiscard]] std::optional<CursorPosition> cursor_location() const override;

    /// Resolve width, height limit, anchor, offsets, and margins for a viewport.
    [[nodiscard]] OverlayLayout resolve_layout(TerminalDimensions viewport, std::size_t content_height) const;

    /// Returns true when this overlay is visible at the viewport dimensions.
    [[nodiscard]] bool visible_at(TerminalDimensions viewport) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace cch::tui

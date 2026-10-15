#pragma once

#include <cch/tui/Component.hpp>
#include <cch/tui/Overlay.hpp>
#include <cch/tui/OverlayHandle.hpp>
#include <cch/tui/Terminal.hpp>

#include <cch/support/Error.hpp>

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace cch::tui::detail {

/// Deep overlay compositing engine for the TUI root. Owns the overlay stack:
/// overlay storage and lifetime, z-ordered responsive visibility, 2D viewport
/// layout and clipping, ANSI-preserving line splicing, in-memory image
/// intersection clipping and retained overlay metadata. Focus transitions
/// remain owned by MainScreen and AltScreen.
///
/// The compositor is pure presentation state and performs no terminal I/O.
/// Focus application (mutating Focusable state) stays with MainScreen and
/// AltScreen, which supply the current focus and apply the compositor's focus decisions.
class OverlayCompositor final {
public:
    OverlayCompositor() = default;
    OverlayCompositor(OverlayCompositor&&) noexcept = default;
    OverlayCompositor& operator=(OverlayCompositor&&) noexcept = default;
    ~OverlayCompositor() = default;

    OverlayCompositor(const OverlayCompositor&) = delete;
    OverlayCompositor& operator=(const OverlayCompositor&) = delete;

    // --- Overlay lifetime ---

    /// Take ownership of an overlay. A null overlay is a validation error.
    [[nodiscard]] support::Expected<std::reference_wrapper<Overlay>> add_overlay(std::unique_ptr<Overlay> overlay,
            Component* pre_focus = nullptr,
            std::size_t focus_order = 0,
            std::shared_ptr<OverlayHandleState> handle_state = {});

    /// Dispose of an owned overlay. A null pointer is a no-op; an overlay not
    /// owned by this compositor is a validation error.
    [[nodiscard]] support::ExpectedVoid remove(Overlay* overlay);

    [[nodiscard]] bool owns(const Overlay* overlay) const;
    [[nodiscard]] bool is_hidden(const Overlay* overlay) const;
    [[nodiscard]] bool set_hidden(Overlay* overlay, bool hidden);
    [[nodiscard]] bool visible(const Overlay* overlay, TerminalDimensions viewport) const;
    [[nodiscard]] Component* pre_focus(const Overlay* overlay) const;
    [[nodiscard]] std::optional<OverlayBounds> bounds(const Overlay* overlay) const;
    void set_focus_order(Overlay* overlay, std::size_t order);
    void retarget_pre_focus(Overlay* removed, Component* replacement);

    void invalidate_all();

    // --- Compositing ---

    /// Scale image sidecars into cell space and reserve their line real
    /// estate. Shared by the main-screen renderer and overlay compositing.
    [[nodiscard]] static RenderResult materialize_images(
        RenderResult output,
        const TerminalCapabilities& capabilities,
        std::size_t width,
        std::size_t available_rows);

    /// Splice every visible overlay (z ascending) into the composed buffer:
    /// viewport layout, ANSI-preserving line splicing, and image intersection
    /// clipping against overlaid regions.
    [[nodiscard]] support::ExpectedVoid composite(
            TerminalDimensions dimensions, const TerminalCapabilities& capabilities, RenderResult& output);

    /// Whether any overlay is visible at these dimensions. Rows are only
    /// reusable while this is false: compositing splices overlays into
    /// composed rows, so the buffer must stay in its prepared form.
    [[nodiscard]] bool has_visible_overlays(TerminalDimensions dimensions) const;

    /// Whether any overlay entry exists (visible or hidden). Used to gate clearOnShrink.
    [[nodiscard]] bool has_overlay_entries() const noexcept { return !overlays_.empty(); }

    // --- Input ---

    /// Legacy compositor probe retained for standalone routing tests.
    [[nodiscard]] InputAdmissionOutcome dispatch_input(
            const InputEventVariant& event, TerminalDimensions viewport) const;

    // --- Focus ---

    /// Topmost visible, capturing overlay eligible for fallback focus.
    [[nodiscard]] Overlay* topmost_focusable(TerminalDimensions viewport) const;

    /// Whether `component` names an owned, visible, capturing overlay.
    [[nodiscard]] bool focus_target_available(Component* component, TerminalDimensions viewport) const;

    /// Record and query legacy focus targets while existing component-only
    /// callers migrate to OverlayHandle.
    void remember_focus(Overlay* overlay, Component* previous);
    [[nodiscard]] Component* return_focus(const Overlay* overlay) const;
    void forget_focus(Overlay* overlay, Component* replacement);
    void clear_focus_history();

    /// Retarget stored pre-focus references when their component is removed.
    void forget_component(Component* component);

private:
    struct OverlayFocusEntry {
        Overlay* overlay;
        Component* previous;
    };

    struct OverlayEntry {
        std::unique_ptr<Overlay> overlay;
        Component* pre_focus{nullptr};
        std::size_t focus_order{0};
        bool hidden{false};
        mutable std::optional<OverlayBounds> bounds;
        std::shared_ptr<OverlayHandleState> handle_state;
    };

    /// Visible overlays sorted by z_index ascending (draw order).
    [[nodiscard]] std::vector<const OverlayEntry*> sorted_visible(TerminalDimensions viewport) const;
    [[nodiscard]] OverlayEntry* find_entry(const Overlay* overlay);
    [[nodiscard]] const OverlayEntry* find_entry(const Overlay* overlay) const;

    std::vector<OverlayEntry> overlays_;
    std::vector<OverlayFocusEntry> focus_history_;
    std::size_t focus_order_counter_{0};
};

} // namespace cch::tui::detail

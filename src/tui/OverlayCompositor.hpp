#pragma once

#include <cch/tui/Component.hpp>
#include <cch/tui/Overlay.hpp>
#include <cch/tui/Terminal.hpp>

#include <cch/support/Error.hpp>

#include <cstddef>
#include <functional>
#include <memory>
#include <vector>

namespace cch::tui::detail {

/// Deep overlay compositing engine for the TUI root. Owns the overlay stack:
/// overlay storage and lifetime, z-ordered responsive visibility, 2D viewport
/// layout and clipping, ANSI-preserving line splicing, in-memory image
/// intersection clipping, and the overlay focus history stack.
///
/// The compositor is pure presentation state and performs no terminal I/O.
/// Focus application (mutating Focusable state) stays with Tui, which supplies
/// the current focus and applies the compositor's focus decisions.
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
    [[nodiscard]] support::Expected<std::reference_wrapper<Overlay>> add_overlay(
        std::unique_ptr<Overlay> overlay);

    /// Dispose of an owned overlay. A null pointer is a no-op; an overlay not
    /// owned by this compositor is a validation error. Callers resolve focus
    /// through the focus history operations around removal.
    [[nodiscard]] support::ExpectedVoid remove(Overlay* overlay);

    [[nodiscard]] bool owns(const Overlay* overlay) const;

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
        TerminalDimensions dimensions,
        const TerminalCapabilities& capabilities,
        RenderResult& output) const;

    // --- Input ---

    /// Dispatch to the topmost visible capturing InputHandler overlay
    /// (reverse z-order). Returns true when an overlay consumed the event.
    bool dispatch_input(const InputEventVariant& event, TerminalDimensions viewport) const;

    // --- Focus history ---

    /// Topmost visible, capturing overlay eligible for fallback focus.
    [[nodiscard]] Overlay* topmost_focusable(TerminalDimensions viewport) const;

    /// Whether `component` names an owned, visible, capturing overlay.
    [[nodiscard]] bool focus_target_available(Component* component, TerminalDimensions viewport) const;

    /// Record that `overlay` took focus from `previous` (null allowed).
    void remember_focus(Overlay* overlay, Component* previous);

    /// The focus target recorded when `overlay` last took focus, or null.
    [[nodiscard]] Component* return_focus(const Overlay* overlay) const;

    /// Drop `overlay`'s history entry and re-point entries that returned
    /// focus to it at `replacement`.
    void forget_focus(Overlay* overlay, Component* replacement);

    void clear_focus_history();

private:
    struct OverlayFocusEntry {
        Overlay* overlay; // aliases an element owned by overlays_.
        Component* previous; // null or aliases a Tui child or an element owned by overlays_.
    };

    /// Visible overlays sorted by z_index ascending (draw order).
    [[nodiscard]] std::vector<Overlay*> sorted_visible(TerminalDimensions viewport) const;

    std::vector<std::unique_ptr<Overlay>> overlays_;
    std::vector<OverlayFocusEntry> focus_history_;
};

} // namespace cch::tui::detail

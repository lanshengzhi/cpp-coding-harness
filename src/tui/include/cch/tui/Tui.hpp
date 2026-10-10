#pragma once

#include <cch/tui/Component.hpp>
#include <cch/tui/Overlay.hpp>
#include <cch/tui/RawInputListener.hpp>
#include <cch/tui/Terminal.hpp>

#include <cch/support/Error.hpp>

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace cch::tui {

class Tui;

/// Notification that a render should be scheduled. Calls are coalesced until
/// the next successful render; the sink must return promptly and must not render inline.
/// A reported failure is a best-effort scheduling notification: it is not recorded and
/// cannot veto input delivery or rendering.
using TuiRenderRequestSink = std::move_only_function<support::ExpectedVoid()>;

namespace detail {
class TerminalStreamDecoder;
class OverlayCompositor;
class RenderPipeline;
} // namespace detail

namespace detail::testing {

/// Test-only accessor for the frame-level line preparation count of the last
/// render. Defined in `Tui.cpp`; see `TuiTestHooks.hpp`.
[[nodiscard]] std::size_t frame_prepare_call_count(const Tui& tui) noexcept;

} // namespace detail::testing

class Tui final {
public:
    explicit Tui(Terminal& terminal);
    Tui(Tui&&) = delete;
    Tui& operator=(Tui&&) = delete;
    ~Tui();

    Tui(const Tui&) = delete;
    Tui& operator=(const Tui&) = delete;

    [[nodiscard]] support::Expected<std::reference_wrapper<Component>> add_child(
        std::unique_ptr<Component> component);
    [[nodiscard]] support::ExpectedVoid start();
    [[nodiscard]] support::ExpectedVoid stop();
    [[nodiscard]] support::ExpectedVoid render();
    /// Clear the physical screen and reset differential-render state so the
    /// next render repaints its complete current presentation.
    [[nodiscard]] support::ExpectedVoid clear_screen();
    [[nodiscard]] support::ExpectedVoid set_focus(Component* component);
    void set_render_request_sink(TuiRenderRequestSink sink);
    void invalidate();

    /// Register an ordered raw-input listener that observes every raw input
    /// fragment before typed dispatch (pi `addInputListener`). The returned
    /// handle removes its registration when destroyed, moved from, or reset;
    /// the caller may also remove it explicitly by id.
    [[nodiscard]] RawInputListenerHandle add_raw_input_listener(RawInputListener listener);
    /// Remove a raw-input listener registration. Removing during a dispatch
    /// takes effect for the rest of that dispatch and every later input.
    [[nodiscard]] support::ExpectedVoid remove_raw_input_listener(RawInputListenerId id);

    /// Add an overlay. Overlays are rendered on top of base children
    /// and support position strategies, stacking, and focus isolation.
    [[nodiscard]] support::Expected<std::reference_wrapper<Overlay>> add_overlay(
        std::unique_ptr<Overlay> overlay);

    /// Remove (dispose) an overlay. Focus falls back to the next
    /// available overlay or base component.
    [[nodiscard]] support::ExpectedVoid remove_overlay(Overlay* overlay);

    /// Hide an overlay. Focus falls back to the next available target.
    [[nodiscard]] support::ExpectedVoid hide_overlay(Overlay* overlay);

    /// Restore (show) a previously hidden overlay.
    [[nodiscard]] support::ExpectedVoid restore_overlay(Overlay* overlay);

private:
    friend std::size_t detail::testing::frame_prepare_call_count(const Tui& tui) noexcept;

    [[nodiscard]] bool owns(const Component* component) const;
    [[nodiscard]] support::Expected<RenderResult> render_children(TerminalDimensions dimensions, bool prepare_rows);
    void handle_input(std::string input);
    void dispatch_input(const InputEventVariant& event);
    void handle_resize(TerminalDimensions dimensions);
    void apply_focus(Component* component);
    [[nodiscard]] bool focus_target_available(Component* component) const;
    void fallback_focus();
    [[nodiscard]] Focusable* find_focusable_target();
    [[nodiscard]] std::optional<CursorPosition> resolve_cursor_location() const;

    Terminal& terminal_; // must outlive this Tui.
    std::unique_ptr<detail::TerminalStreamDecoder> stream_decoder_;
    std::unique_ptr<detail::OverlayCompositor> compositor_;
    std::unique_ptr<detail::RenderPipeline> render_pipeline_;
    TuiRenderRequestSink render_request_sink_;
    std::shared_ptr<RawInputListenerChain> raw_listeners_;
    std::vector<std::unique_ptr<Component>> children_;
    Component* focused_{nullptr}; // Null or aliases an element owned by children_ or the compositor's overlays.
    bool started_{false};
    bool pending_render_{false};
};

} // namespace cch::tui

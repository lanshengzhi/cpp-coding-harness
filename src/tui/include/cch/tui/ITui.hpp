#pragma once

#include <cch/tui/Component.hpp>
#include <cch/tui/Overlay.hpp>
#include <cch/tui/OverlayHandle.hpp>
#include <cch/tui/Keybindings.hpp>
#include <cch/tui/RawInputListener.hpp>
#include <cch/tui/Terminal.hpp>
#include <cch/tui/TerminalColors.hpp>
#include <cch/tui/TuiDiagnostics.hpp>
#include <cch/tui/TuiScheduler.hpp>

#include <cch/support/Error.hpp>

#include <cstddef>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace cch::tui {

/// Options for restoring terminal state when a TUI stops.
struct TuiStopOptions {
    /// Leave the current screen content and cursor position in place for another TUI to take over.
    bool preserve_screen{false};
    /// Renderer handoff only: detach pending color queries from input routing,
    /// retaining their completion and partial colors until the original deadline.
    /// Default stop and destruction still cancel. Callbacks must own
    /// lifetime-safe state independent of the retired root.
    bool preserve_pending_color_query_completion{false};
};

using TuiRenderPrepareSink = std::move_only_function<support::Expected<bool>()>;
using TuiRenderCompleteSink = std::move_only_function<void(const support::ExpectedVoid&)>;

/// Common lifecycle and presentation surface for regular and fullscreen TUI roots.
/// Mode-specific terminal operations remain on their concrete implementation.
class ITui {
public:
    virtual ~ITui() = default;

    [[nodiscard]] virtual Terminal& terminal() noexcept = 0;
    [[nodiscard]] virtual const Terminal& terminal() const noexcept = 0;
    /// Update the live keybinding snapshot used by mode-specific root actions.
    /// Regular-screen roots do not consume viewport bindings.
    virtual void set_keybinding_context(std::shared_ptr<KeybindingsContext>) {}

    [[nodiscard]] virtual support::Expected<std::reference_wrapper<Component>> add_child(
            std::unique_ptr<Component> component) = 0;
    [[nodiscard]] virtual support::ExpectedVoid remove_child(Component* component) = 0;
    virtual void clear_children() = 0;

    /// Detach every mounted child and return it without destroying it (pi
    /// `TuiBase.children` capture plus `clear()`, `tui.ts:362`): the caller
    /// owns the returned components and may mount them on another root, which
    /// is how a product keeps one component tree across a renderer switch.
    [[nodiscard]] virtual std::vector<std::unique_ptr<Component>> take_children() = 0;

    /// Whether any overlay entry exists, hidden entries included (pi
    /// `TuiBase.hasOverlayEntries`, `tui.ts:527`).
    [[nodiscard]] virtual bool has_overlay_entries() const noexcept = 0;

    /// Clear all overlay entries, restoring any pre-focus state.
    virtual void clear_overlays() = 0;

    [[nodiscard]] virtual support::ExpectedVoid set_focus(Component* component) = 0;
    [[nodiscard]] virtual Component* focus() const noexcept = 0;

    [[nodiscard]] virtual support::ExpectedVoid start() = 0;
    [[nodiscard]] virtual support::ExpectedVoid stop(TuiStopOptions options = {}) = 0;
    [[nodiscard]] virtual support::ExpectedVoid render() = 0;
    /// Render one final presentation frame while unstarted (exit transcript output).
    /// Emits rendered components in output-only terminal mode without starting input loops.
    [[nodiscard]] virtual support::ExpectedVoid render_final_presentation() = 0;
    [[nodiscard]] virtual support::ExpectedVoid clear_screen() = 0;
    virtual void request_render(bool force = false) = 0;
    virtual void request_immediate_render() = 0;
    virtual void invalidate() = 0;
    virtual void set_clear_on_shrink(bool enabled) noexcept = 0;
    [[nodiscard]] virtual bool clear_on_shrink() const noexcept = 0;

    /// Hardware-cursor visibility preference (pi `getShowHardwareCursor` / `setShowHardwareCursor`).
    /// When disabled (default `false`), the hardware cursor remains hidden during
    /// execution while still being positioned at the active component cursor for IME.
    /// When enabled, the hardware cursor is made visible whenever a valid cursor
    /// location is resolved, and hidden when no cursor is present.
    [[nodiscard]] virtual bool show_hardware_cursor() const noexcept = 0;
    virtual void set_show_hardware_cursor(bool enabled) = 0;

    /// Parity alias for `show_hardware_cursor()`.
    [[nodiscard]] bool get_show_hardware_cursor() const noexcept { return show_hardware_cursor(); }

    virtual void set_scheduler(TuiScheduler* scheduler) = 0;
    virtual void set_render_prepare_sink(TuiRenderPrepareSink sink) = 0;
    virtual void set_render_complete_sink(TuiRenderCompleteSink sink) = 0;

    /// Registers the debug-key callback (pi `TuiBase.onDebug`). The key is
    /// matched after the raw-input listener stage and before focus reconciliation,
    /// and is consumed while a callback is set. The callback survives stop/start.
    virtual void set_on_debug(TuiDebugHook hook) = 0;
    /// Full-redraw attempts over this root's lifetime (pi `TuiBase.fullRedraws`).
    /// The count advances when a full redraw is attempted, not only when it commits.
    [[nodiscard]] virtual std::size_t full_redraws() const noexcept = 0;
    /// Terminal write accounting of the most recent frame; zero before the first frame.
    [[nodiscard]] virtual TuiWriteAccounting write_accounting() const noexcept = 0;

    [[nodiscard]] virtual RawInputListenerHandle add_raw_input_listener(RawInputListener listener) = 0;
    [[nodiscard]] virtual support::ExpectedVoid remove_raw_input_listener(RawInputListenerId id) = 0;

    /// Registers a listener for live terminal color-scheme notifications (CSI ? 997 ; 1|2 n).
    [[nodiscard]] virtual TerminalColorSchemeListenerHandle on_terminal_color_scheme_change(
            TerminalColorSchemeListener listener) = 0;

    /// Enables or disables live terminal color-scheme notifications (DECSET/DECRST 2031).
    /// Writes \x1b[?2031h / \x1b[?2031l to the terminal.
    virtual void set_terminal_color_scheme_notifications(bool enabled) = 0;

    /// Queries foreground, background, and palette colors from the terminal via OSC 10/11/4 and DA1.
    [[nodiscard]] virtual std::future<TerminalColors> query_terminal_colors(TerminalColorsQueryOptions options) = 0;

    /// Convenience overload querying terminal colors with a default timeout and no callbacks.
    [[nodiscard]] std::future<TerminalColors> query_terminal_colors(std::chrono::milliseconds timeout) {
        return query_terminal_colors(TerminalColorsQueryOptions{.timeout = timeout});
    }

    [[nodiscard]] virtual support::Expected<OverlayHandle> show_overlay(std::unique_ptr<Overlay> overlay) = 0;
    [[nodiscard]] virtual support::Expected<std::reference_wrapper<Overlay>> add_overlay(
            std::unique_ptr<Overlay> overlay) = 0;
    [[nodiscard]] virtual support::ExpectedVoid remove_overlay(Overlay* overlay) = 0;
    [[nodiscard]] virtual support::ExpectedVoid hide_overlay(Overlay* overlay) = 0;
    [[nodiscard]] virtual support::ExpectedVoid restore_overlay(Overlay* overlay) = 0;
};

} // namespace cch::tui

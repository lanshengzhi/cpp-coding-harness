#pragma once

#include <cch/tui/Component.hpp>
#include <cch/tui/Overlay.hpp>
#include <cch/tui/OverlayHandle.hpp>
#include <cch/tui/RawInputListener.hpp>
#include <cch/tui/Terminal.hpp>
#include <cch/tui/TerminalColors.hpp>
#include <cch/tui/TuiScheduler.hpp>

#include <cch/support/Error.hpp>

#include <functional>
#include <memory>

namespace cch::tui {

using TuiRenderPrepareSink = std::move_only_function<support::Expected<bool>()>;
using TuiRenderCompleteSink = std::move_only_function<void(const support::ExpectedVoid&)>;

/// Common lifecycle and presentation surface for regular and fullscreen TUI roots.
/// Mode-specific terminal operations remain on their concrete implementation.
class ITui {
public:
    virtual ~ITui() = default;

    [[nodiscard]] virtual Terminal& terminal() noexcept = 0;
    [[nodiscard]] virtual const Terminal& terminal() const noexcept = 0;

    [[nodiscard]] virtual support::Expected<std::reference_wrapper<Component>> add_child(
            std::unique_ptr<Component> component) = 0;
    [[nodiscard]] virtual support::ExpectedVoid remove_child(Component* component) = 0;
    virtual void clear_children() = 0;

    [[nodiscard]] virtual support::ExpectedVoid set_focus(Component* component) = 0;
    [[nodiscard]] virtual Component* focus() const noexcept = 0;

    [[nodiscard]] virtual support::ExpectedVoid start() = 0;
    [[nodiscard]] virtual support::ExpectedVoid stop() = 0;
    [[nodiscard]] virtual support::ExpectedVoid render() = 0;
    [[nodiscard]] virtual support::ExpectedVoid clear_screen() = 0;
    virtual void request_render(bool force = false) = 0;
    virtual void request_immediate_render() = 0;
    virtual void invalidate() = 0;
    virtual void set_scheduler(TuiScheduler* scheduler) = 0;
    virtual void set_render_prepare_sink(TuiRenderPrepareSink sink) = 0;
    virtual void set_render_complete_sink(TuiRenderCompleteSink sink) = 0;

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

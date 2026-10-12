#pragma once

#include <cch/tui/ITui.hpp>
#include <cch/tui/TuiHost.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace cch::tui {

/// Absolute-viewport render state of an alternate-screen root, for transfer
/// between roots (#985 shape). `width == 0` means no baseline was captured;
/// use `reset_render_state()` for the -1 sentinel that forces the next frame
/// to clear and repaint.
struct TuiAltScreenRenderState {
    std::vector<std::string> viewport_rows;
    std::size_t width{0};
    std::size_t height{0};
    std::optional<CursorPosition> cursor;
};

/// Alternate-screen (fullscreen) root: owns the terminal's alternate buffer
/// for exactly the lifetime between a successful `start()` and `stop()`.
///
/// The viewport is absolute: every frame repaints only the rows that differ
/// from the committed baseline, each row is written at its own screen position
/// and rows past the terminal height are clipped instead of appended, so
/// content never enters scrollback. `stop()` exits the alternate buffer so the
/// main buffer returns, whether it is called normally or after a failed
/// start; `TuiStopOptions::preserve_screen` does not apply to this root.
///
/// Overlays are not implemented on this root yet (#998/#999): every overlay
/// entry point fails validation until the deferred overlay chain lands.
class AltScreen final : public ITui, public detail::TuiHost {
public:
    explicit AltScreen(Terminal& terminal);
    ~AltScreen() override;

    AltScreen(const AltScreen&) = delete;
    AltScreen& operator=(const AltScreen&) = delete;

    [[nodiscard]] Terminal& terminal() noexcept override { return terminal_; }
    [[nodiscard]] const Terminal& terminal() const noexcept override { return terminal_; }

    [[nodiscard]] support::Expected<std::reference_wrapper<Component>> add_child(
            std::unique_ptr<Component> component) override;
    [[nodiscard]] support::ExpectedVoid remove_child(Component* component) override;
    void clear_children() override;

    [[nodiscard]] support::ExpectedVoid set_focus(Component* component) override;
    [[nodiscard]] Component* focus() const noexcept override { return focused_; }

    [[nodiscard]] support::ExpectedVoid start() override;
    [[nodiscard]] support::ExpectedVoid stop(TuiStopOptions options = {}) override;
    [[nodiscard]] support::ExpectedVoid render() override;
    [[nodiscard]] support::ExpectedVoid clear_screen() override;
    void request_render(bool force = false) override;
    void request_immediate_render() override;
    void invalidate() override;
    void set_clear_on_shrink(bool enabled) noexcept override;
    [[nodiscard]] bool clear_on_shrink() const noexcept override;
    void set_scheduler(TuiScheduler* scheduler) override;
    void set_render_prepare_sink(TuiRenderPrepareSink sink) override;
    void set_render_complete_sink(TuiRenderCompleteSink sink) override;

    [[nodiscard]] RawInputListenerHandle add_raw_input_listener(RawInputListener listener) override;
    [[nodiscard]] support::ExpectedVoid remove_raw_input_listener(RawInputListenerId id) override;
    [[nodiscard]] TerminalColorSchemeListenerHandle on_terminal_color_scheme_change(
            TerminalColorSchemeListener listener) override;
    void set_terminal_color_scheme_notifications(bool enabled) override;
    [[nodiscard]] std::future<TerminalColors> query_terminal_colors(TerminalColorsQueryOptions options) override;

    [[nodiscard]] support::Expected<OverlayHandle> show_overlay(std::unique_ptr<Overlay> overlay) override;
    [[nodiscard]] support::Expected<std::reference_wrapper<Overlay>> add_overlay(
            std::unique_ptr<Overlay> overlay) override;
    [[nodiscard]] support::ExpectedVoid remove_overlay(Overlay* overlay) override;
    [[nodiscard]] support::ExpectedVoid hide_overlay(Overlay* overlay) override;
    [[nodiscard]] support::ExpectedVoid restore_overlay(Overlay* overlay) override;

    /// Committed viewport baseline: the prepared rows and cursor of the last
    /// admitted frame.
    [[nodiscard]] TuiAltScreenRenderState capture_render_state() const;
    /// Import a captured baseline. Rejected while the root is running; a state
    /// with `width == 0` restores the construction-time "no baseline" case.
    [[nodiscard]] support::ExpectedVoid restore_render_state(const TuiAltScreenRenderState& state);
    /// Drop the baseline and force the next frame through the clearing
    /// full-redraw path (the -1 sentinel; distinct from a fresh root).
    void reset_render_state() noexcept;

private:
    struct RenderScheduleState {
        AltScreen* owner{nullptr};
    };

    [[nodiscard]] bool owns(const Component* component) const;
    [[nodiscard]] support::ExpectedVoid render_now();
    [[nodiscard]] support::ExpectedVoid render_frame();
    [[nodiscard]] support::ExpectedVoid render_children(
            TerminalDimensions dimensions, std::vector<std::string>& lines, std::optional<CursorPosition>& cursor);
    [[nodiscard]] support::ExpectedVoid render_viewport(
            const std::vector<std::string>& lines, std::optional<CursorPosition> cursor, TerminalDimensions dimensions);
    void dispatch_input(const InputEventVariant& event);
    void handle_input(std::string input);
    void handle_resize(TerminalDimensions dimensions);
    void apply_focus(Component* component);
    [[nodiscard]] bool focus_target_available(Component* component) const;
    void fallback_focus();
    void schedule_render();
    void cancel_scheduled_render() noexcept;
    void execute_scheduled_render(std::size_t generation);

    std::shared_ptr<RenderScheduleState> render_schedule_state_;
    TuiScheduler* scheduler_{nullptr};
    TuiRenderPrepareSink render_prepare_sink_;
    TuiRenderCompleteSink render_complete_sink_;
    bool started_{false};
    bool stopping_{false};
    bool pending_render_{false};
    bool render_scheduled_{false};
    bool clear_on_shrink_{false};
    std::size_t render_generation_{0};

    // Absolute-viewport baseline. Construction starts with no baseline and
    // paints every row without clearing; `reset_pending_` is the sentinel set
    // by `reset_render_state()` that additionally clears the buffer first.
    bool first_render_{true};
    bool reset_pending_{false};
    // Set by restore_render_state() so the next start() keeps the imported
    // baseline instead of resetting it for the freshly entered buffer.
    bool baseline_from_restore_{false};
    std::optional<TerminalDimensions> previous_dimensions_;
    std::vector<std::string> previous_lines_;
    std::optional<CursorPosition> cursor_;
    bool cursor_visible_{false};
};

} // namespace cch::tui

#pragma once

#include <cch/tui/ITui.hpp>

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace cch::tui {

class MainScreen;

namespace detail {
class TerminalStreamDecoder;
class OverlayCompositor;
class RenderPipeline;
} // namespace detail

namespace detail::testing {

/// Test-only accessor for the frame-level line preparation count of the last
/// render. Defined in `Tui.cpp`; see `TuiTestHooks.hpp`.
[[nodiscard]] std::size_t frame_prepare_call_count(const MainScreen& tui) noexcept;

} // namespace detail::testing

/// Regular-mode root implementation. `Tui` remains an alias for source
/// compatibility while consumers use the shared ITui contract.
class MainScreen final : public ITui {
public:
    explicit MainScreen(Terminal& terminal);
    MainScreen(MainScreen&&) = delete;
    MainScreen& operator=(MainScreen&&) = delete;
    ~MainScreen() override;

    MainScreen(const MainScreen&) = delete;
    MainScreen& operator=(const MainScreen&) = delete;

    [[nodiscard]] Terminal& terminal() noexcept override { return terminal_; }
    [[nodiscard]] const Terminal& terminal() const noexcept override { return terminal_; }

    [[nodiscard]] support::Expected<std::reference_wrapper<Component>> add_child(
            std::unique_ptr<Component> component) override;
    [[nodiscard]] support::ExpectedVoid remove_child(Component* component) override;
    void clear_children() override;

    [[nodiscard]] support::ExpectedVoid start() override;
    [[nodiscard]] support::ExpectedVoid stop() override;
    [[nodiscard]] support::ExpectedVoid render() override;
    /// Clear the physical screen and reset differential-render state so the
    /// next render repaints its complete current presentation.
    [[nodiscard]] support::ExpectedVoid clear_screen() override;
    [[nodiscard]] support::ExpectedVoid set_focus(Component* component) override;
    [[nodiscard]] Component* focus() const noexcept override { return focused_; }
    void request_render(bool force = false) override;
    void request_immediate_render() override;
    void invalidate() override;
    void set_scheduler(TuiScheduler* scheduler) override;
    void set_render_prepare_sink(TuiRenderPrepareSink sink) override;
    void set_render_complete_sink(TuiRenderCompleteSink sink) override;

    /// Register an ordered raw-input listener that observes every raw input
    /// fragment before typed dispatch (pi `addInputListener`). The returned
    /// handle removes its registration when destroyed, moved from, or reset;
    /// the caller may also remove it explicitly by id.
    [[nodiscard]] RawInputListenerHandle add_raw_input_listener(RawInputListener listener) override;
    /// Remove a raw-input listener registration. Removing during a dispatch
    /// takes effect for the rest of that dispatch and every later input.
    [[nodiscard]] support::ExpectedVoid remove_raw_input_listener(RawInputListenerId id) override;

    /// Add an overlay. Overlays are rendered on top of base children
    /// and support position strategies, stacking, and focus isolation.
    [[nodiscard]] support::Expected<OverlayHandle> show_overlay(std::unique_ptr<Overlay> overlay) override;
    [[nodiscard]] support::Expected<std::reference_wrapper<Overlay>> add_overlay(
            std::unique_ptr<Overlay> overlay) override;

    /// Remove (dispose) an overlay. Focus falls back to the next
    /// available overlay or base component.
    [[nodiscard]] support::ExpectedVoid remove_overlay(Overlay* overlay) override;

    /// Hide an overlay. Focus falls back to the next available target.
    [[nodiscard]] support::ExpectedVoid hide_overlay(Overlay* overlay) override;

    /// Restore (show) a previously hidden overlay.
    [[nodiscard]] support::ExpectedVoid restore_overlay(Overlay* overlay) override;

private:
    friend class OverlayHandle;
    friend std::size_t detail::testing::frame_prepare_call_count(const MainScreen& tui) noexcept;

    class ChildOperation final {
    public:
        explicit ChildOperation(MainScreen& owner) noexcept : owner_(owner) { ++owner_.child_operation_depth_; }
        ~ChildOperation() { owner_.finish_child_operation(); }

        ChildOperation(const ChildOperation&) = delete;
        ChildOperation& operator=(const ChildOperation&) = delete;

    private:
        MainScreen& owner_;
    };

    struct RenderScheduleState;
    enum class OverlayFocusRestoreStatus { Inactive, Eligible, Blocked };
    struct OverlayFocusRestoreState {
        OverlayFocusRestoreStatus status{OverlayFocusRestoreStatus::Inactive};
        Overlay* overlay{nullptr};
        Component* blocked_by{nullptr};
        bool resume_to_overlay{true};
        Component* resume_target{nullptr};
    };

    void finish_child_operation() noexcept;
    [[nodiscard]] support::ExpectedVoid render_now();
    [[nodiscard]] support::ExpectedVoid render_frame();
    void schedule_render();
    [[nodiscard]] bool owns(const Component* component) const;
    [[nodiscard]] support::Expected<RenderResult> render_children(TerminalDimensions dimensions, bool prepare_rows);
    void handle_input(std::string input);
    void dispatch_input(const InputEventVariant& event);
    void handle_resize(TerminalDimensions dimensions);
    void cancel_scheduled_render() noexcept;
    void execute_scheduled_render(std::size_t generation);
    void apply_focus(Component* component);
    void set_focus_internal(Component* component, bool preserve_restore);
    void remove_overlay_internal(Overlay* overlay);
    void hide_overlay_internal(Overlay* overlay, bool hidden);
    void focus_overlay_internal(Overlay* overlay);
    void unfocus_overlay_internal(Overlay* overlay, std::optional<Component*> target);
    void reconcile_overlay_focus(TerminalDimensions dimensions);
    void clear_overlay_restore_for(Overlay* overlay);
    [[nodiscard]] Component* resolve_overlay_restore();
    [[nodiscard]] bool focus_target_available(Component* component) const;
    void fallback_focus();
    [[nodiscard]] Focusable* find_focusable_target();
    [[nodiscard]] std::optional<CursorPosition> resolve_cursor_location() const;

    Terminal& terminal_; // must outlive this MainScreen.
    std::shared_ptr<RenderScheduleState> render_schedule_state_;
    std::shared_ptr<detail::OverlayHandleContext> overlay_handle_context_;
    TuiScheduler* scheduler_{nullptr}; // must outlive every scheduled render.
    TuiRenderPrepareSink render_prepare_sink_;
    TuiRenderCompleteSink render_complete_sink_;
    std::optional<std::chrono::steady_clock::time_point> last_render_at_;
    std::size_t render_generation_{0};
    bool render_scheduled_{false};
    std::unique_ptr<detail::TerminalStreamDecoder> stream_decoder_;
    std::unique_ptr<detail::OverlayCompositor> compositor_;
    std::unique_ptr<detail::RenderPipeline> render_pipeline_;
    std::shared_ptr<RawInputListenerChain> raw_listeners_;
    std::vector<std::unique_ptr<Component>> children_;
    std::vector<std::unique_ptr<Component>> retired_children_;
    std::vector<std::size_t> rendered_child_rows_;
    bool rendered_children_valid_{false};
    std::size_t child_operation_depth_{0};
    Component* focused_{nullptr}; // Null or aliases an element owned by children_ or the compositor's overlays.
    bool started_{false};
    bool stopping_{false};
    bool pending_render_{false};
    std::size_t focus_order_counter_{0};
    OverlayFocusRestoreState overlay_focus_restore_;
};

using Tui = MainScreen;

} // namespace cch::tui

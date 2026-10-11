#pragma once

#include <cch/tui/ITui.hpp>

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
    void set_render_request_sink(TuiRenderRequestSink sink) override;
    void request_render() override;
    void invalidate() override;

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

    void finish_child_operation() noexcept;
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

    Terminal& terminal_; // must outlive this MainScreen.
    std::unique_ptr<detail::TerminalStreamDecoder> stream_decoder_;
    std::unique_ptr<detail::OverlayCompositor> compositor_;
    std::unique_ptr<detail::RenderPipeline> render_pipeline_;
    TuiRenderRequestSink render_request_sink_;
    std::shared_ptr<RawInputListenerChain> raw_listeners_;
    std::vector<std::unique_ptr<Component>> children_;
    std::vector<std::unique_ptr<Component>> retired_children_;
    std::size_t child_operation_depth_{0};
    Component* focused_{nullptr}; // Null or aliases an element owned by children_ or the compositor's overlays.
    bool started_{false};
    bool stopping_{false};
    bool pending_render_{false};
};

using Tui = MainScreen;

} // namespace cch::tui

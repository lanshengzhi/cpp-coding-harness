#pragma once

#include <cch/tui/Component.hpp>
#include <cch/tui/Overlay.hpp>
#include <cch/tui/Terminal.hpp>

#include <cch/support/Error.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace cch::tui {

/// Notification that a render should be scheduled. Calls are coalesced until
/// the next successful render; the sink must return promptly and must not render inline.
/// A reported failure is a best-effort scheduling notification: it is not recorded and
/// cannot veto input delivery or rendering.
using TuiRenderRequestSink = std::move_only_function<support::ExpectedVoid()>;

namespace detail {
class TerminalStreamDecoder;
class OverlayCompositor;
} // namespace detail

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
    struct ActiveImage {
        TerminalImageHandle handle;
        CellRegion region;
        std::uint64_t resource_id{0};
        std::uint64_t revision{0};
    };

    [[nodiscard]] bool owns(const Component* component) const;
    [[nodiscard]] support::Expected<RenderResult> render_children(TerminalDimensions dimensions);
    [[nodiscard]] support::ExpectedVoid remove_active_images();
    [[nodiscard]] support::ExpectedVoid remove_images_intersecting(const CellRegion& region);
    [[nodiscard]] support::ExpectedVoid remove_stale_images(
        const std::vector<InlineImageRenderRegion>& desired_images);
    [[nodiscard]] support::ExpectedVoid place_images(
        const std::vector<InlineImageRenderRegion>& desired_images);
    void handle_input(std::string input);
    void dispatch_input(const InputEventVariant& event);
    void handle_resize(TerminalDimensions dimensions);
    void apply_focus(Component* component);
    [[nodiscard]] bool focus_target_available(Component* component) const;
    void fallback_focus();
    [[nodiscard]] Focusable* find_focusable_target();
    [[nodiscard]] std::optional<CursorPosition> resolve_cursor_location() const;

    Terminal& terminal_; // must outlive this Tui.
    std::recursive_mutex mutex_;
    std::unique_ptr<detail::TerminalStreamDecoder> stream_decoder_;
    std::unique_ptr<detail::OverlayCompositor> compositor_;
    TuiRenderRequestSink render_request_sink_;
    std::vector<std::unique_ptr<Component>> children_;
    Component* focused_{nullptr}; // Null or aliases an element owned by children_ or the compositor's overlays.
    bool started_{false};
    bool first_render_{true};
    bool pending_render_{false};
    std::vector<std::string> previous_lines_;
    /// Buffer row at the top of the visible viewport under the main-screen
    /// scrollback flow (pi `TuiMainScreen` `previousViewportTop`): the
    /// composed buffer's lines below it are the terminal's native scrollback.
    std::size_t viewport_top_{0};
    std::vector<ActiveImage> active_images_;
    TerminalDimensions previous_dimensions_{};
};

} // namespace cch::tui

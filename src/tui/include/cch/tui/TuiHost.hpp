#pragma once

// Shared interaction base for TUI roots (#997). Owns the state and pipeline
// every root needs independently of its rendering flow: mounted children,
// focus state, ordered raw-input listeners, the input decoder and the
// terminal-color query/scheme plumbing. Screen lifecycle, rendering and
// viewport policy stay on the concrete roots.
//
// Repository-internal base: declared on the owner include root so both roots
// can share it, but `detail` and not part of the stable Owner Interface.

#include <cch/tui/Component.hpp>
#include <cch/tui/Keys.hpp>
#include <cch/tui/RawInputListener.hpp>
#include <cch/tui/Terminal.hpp>
#include <cch/tui/TerminalColors.hpp>
#include <cch/tui/TerminalResponses.hpp>
#include <cch/tui/TuiDiagnostics.hpp>

#include <cch/support/Error.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace cch::tui {

class OverlayHandle;

class Overlay;

namespace detail {

class TerminalStreamDecoder;

class TuiHost {
public:
    /// The overlay handle inspects the focused component through its root.
    friend class ::cch::tui::OverlayHandle;

    enum class OverlayFocusRestoreStatus { Inactive, Eligible, Blocked };
    struct OverlayFocusRestoreState {
        OverlayFocusRestoreStatus status{OverlayFocusRestoreStatus::Inactive};
        Overlay* overlay{nullptr};
        Component* blocked_by{nullptr};
        bool resume_to_overlay{true};
        Component* resume_target{nullptr};
    };

    explicit TuiHost(Terminal& terminal);
    virtual ~TuiHost();
    TuiHost(const TuiHost&) = delete;
    TuiHost& operator=(const TuiHost&) = delete;

    [[nodiscard]] Terminal& terminal() noexcept { return terminal_; }
    [[nodiscard]] const Terminal& terminal() const noexcept { return terminal_; }

    /// Register an ordered raw-input listener (pi `addInputListener`).
    [[nodiscard]] RawInputListenerHandle add_raw_input_listener(RawInputListener listener);
    [[nodiscard]] support::ExpectedVoid remove_raw_input_listener(RawInputListenerId id);
    /// Decode one raw input fragment, run the ordered listener chain and
    /// dispatch surviving typed events. A listener may consume the fragment
    /// or rewrite it (an empty rewrite drops the rest of the fragment).
    void handle_input(std::string input, const std::function<void(const InputEventVariant&)>& dispatch);
    /// Register the debug-key callback (pi `TuiBase.onDebug`). It runs for the
    /// `shift+ctrl+d` key after the raw-input listener chain and before the root
    /// dispatches the event, so the key never reaches the focused component
    /// while a callback is set. The callback is not cleared by stop().
    void set_debug_hook(TuiDebugHook hook);
    /// Drop any pending decoder state (an incomplete escape sequence).
    void reset_decoder() noexcept;

    [[nodiscard]] TerminalColorSchemeListenerHandle on_terminal_color_scheme_change(
            TerminalColorSchemeListener listener);
    /// Store the notification preference and, when the root is running, write
    /// the DECSET/DECRST 2031 toggle. `started`/`stopping` mirror the root's
    /// lifecycle so a preference changed while stopped only takes effect at
    /// the next start().
    void set_terminal_color_scheme_notifications(bool enabled, bool started, bool stopping);
    [[nodiscard]] std::future<TerminalColors> query_terminal_colors(TerminalColorsQueryOptions options);
    /// Cancel every pending color query: resolve its promise with the partial
    /// result, drop its callbacks and wake its timer.
    void cancel_color_queries();

    /// Apply focus state: the previously focused target is unfocused, then the
    /// new target is focused only when `available` accepts it.
    void apply_focus(Component* component, const std::function<bool(Component*)>& available);

protected:
    /// RAII depth counter: retiring a child keeps it alive until the outermost
    /// child operation completes, so a callback cannot observe a freed child.
    class ChildOperation final {
    public:
        explicit ChildOperation(TuiHost& owner) noexcept : owner_(owner) { ++owner_.child_operation_depth_; }
        ~ChildOperation() { owner_.finish_child_operation(); }

        ChildOperation(const ChildOperation&) = delete;
        ChildOperation& operator=(const ChildOperation&) = delete;

    private:
        TuiHost& owner_;
    };

    void finish_child_operation() noexcept;

    struct QueryState {
        std::uint64_t id{0};
        std::mutex mutex;
        std::condition_variable cv;
        std::optional<RgbColor> foreground;
        std::optional<RgbColor> background;
        std::array<std::optional<RgbColor>, 16> palette{};
        std::unordered_set<std::string> replied;
        std::promise<TerminalColors> promise;
        TerminalColorsCallback on_complete;
        TerminalColorsCallback on_late_reply;
        bool promise_resolved{false};
        bool completed{false};
        bool cancelled{false};
        std::chrono::steady_clock::time_point deadline;

        /// Result projection: a palette is reported only when every ANSI
        /// color 0-15 replied.
        [[nodiscard]] static TerminalColors build_terminal_colors(const QueryState& query);
    };

    /// Resolve a timed-out color query from its own state only. Static so the
    /// detached timer thread never needs the root that issued it.
    static void handle_color_query_timeout(const std::shared_ptr<QueryState>& query);
    bool consume_terminal_color_response(const OscColorResponse& response);
    bool consume_terminal_device_attributes();
    void dispatch_color_scheme_report(TerminalColorScheme scheme);
    void apply_terminal_responses(const std::vector<TerminalResponseVariant>& responses);
    void apply_cell_size_responses(const std::vector<TerminalResponseVariant>& responses);
    /// Return true when the debug hook consumed the event.
    [[nodiscard]] bool invoke_debug_hook(const InputEventVariant& event);

    Terminal& terminal_; // must outlive this host.
    std::vector<std::unique_ptr<Component>> children_;
    std::vector<std::unique_ptr<Component>> retired_children_;
    std::size_t child_operation_depth_{0};
    Component* focused_{nullptr}; // Null or aliases a mounted child or an overlay.
    std::size_t focus_order_counter_{0};
    OverlayFocusRestoreState overlay_focus_restore_;
    bool color_scheme_notifications_enabled_{false};
    std::mutex scheme_listeners_mutex_;
    std::vector<std::pair<TerminalColorSchemeListenerId, TerminalColorSchemeListener>> scheme_listeners_;
    TerminalColorSchemeListenerId next_scheme_listener_id_{1};
    std::mutex color_queries_mutex_;
    std::deque<std::shared_ptr<QueryState>> pending_color_queries_;
    std::uint64_t next_query_id_{1};
    std::shared_ptr<RawInputListenerChain> raw_listeners_;
    TuiDebugHook debug_hook_;
    std::unique_ptr<TerminalStreamDecoder> stream_decoder_; // incomplete type; destroyed out of line.
};

} // namespace detail
} // namespace cch::tui

#pragma once

// Shared per-root interaction state for regular and alternate-screen roots.
// Rendering and screen-specific lifecycle remain on the concrete roots.

#include <cch/tui/Component.hpp>
#include <cch/tui/Overlay.hpp>
#include <cch/tui/RawInputListener.hpp>
#include <cch/tui/Terminal.hpp>
#include <cch/tui/Keybindings.hpp>
#include <cch/tui/TerminalColors.hpp>
#include <cch/tui/TerminalResponses.hpp>
#include <cch/support/Error.hpp>

#include "tui/InputDecoder.hpp"

#include <array>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace cch::tui::detail {

class TuiHost final {
public:
    enum class OverlayFocusRestoreStatus { Inactive, Eligible, Blocked };
    struct OverlayFocusRestoreState {
        OverlayFocusRestoreStatus status{OverlayFocusRestoreStatus::Inactive};
        Overlay* overlay{nullptr};
        Component* blocked_by{nullptr};
        bool resume_to_overlay{true};
        Component* resume_target{nullptr};
    };

    explicit TuiHost(Terminal& terminal);
    ~TuiHost();
    TuiHost(const TuiHost&) = delete;
    TuiHost& operator=(const TuiHost&) = delete;

    [[nodiscard]] Terminal& terminal() noexcept { return terminal_; }
    [[nodiscard]] std::vector<std::unique_ptr<Component>>& children() noexcept { return children_; }
    [[nodiscard]] std::vector<std::unique_ptr<Component>>& retired_children() noexcept { return retired_children_; }
    [[nodiscard]] Component*& focused() noexcept { return focused_; }
    [[nodiscard]] const Component* focused() const noexcept { return focused_; }
    [[nodiscard]] OverlayFocusRestoreState& overlay_focus_restore() noexcept { return overlay_focus_restore_; }
    [[nodiscard]] std::size_t& focus_order_counter() noexcept { return focus_order_counter_; }
    [[nodiscard]] std::shared_ptr<KeybindingsContext> keybinding_context() const noexcept {
        return keybinding_context_;
    }
    void set_keybinding_context(std::shared_ptr<KeybindingsContext> context) {
        keybinding_context_ = std::move(context);
    }
    void apply_focus(Component* component, const std::function<bool(Component*)>& available);

    [[nodiscard]] RawInputListenerHandle add_raw_input_listener(RawInputListener listener);
    [[nodiscard]] support::ExpectedVoid remove_raw_input_listener(RawInputListenerId id);
    void set_kitty_protocol_active(bool active) noexcept;
    void reset_decoder() noexcept;
    void handle_input(std::string input, std::function<void(const InputEventVariant&)> dispatch);
    void cancel_color_queries();

    [[nodiscard]] TerminalColorSchemeListenerHandle on_terminal_color_scheme_change(
            TerminalColorSchemeListener listener);
    void set_terminal_color_scheme_notifications(bool enabled, bool started, bool stopping);
    [[nodiscard]] bool color_scheme_notifications_enabled() const noexcept {
        return color_scheme_notifications_enabled_;
    }
    [[nodiscard]] std::future<TerminalColors> query_terminal_colors(TerminalColorsQueryOptions options);

private:
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

        [[nodiscard]] static TerminalColors build_terminal_colors(const QueryState& query);
    };

    static void handle_color_query_timeout(const std::shared_ptr<QueryState>& query);
    [[nodiscard]] bool consume_terminal_color_response(const OscColorResponse& response);
    [[nodiscard]] bool consume_terminal_device_attributes();
    void dispatch_color_scheme_report(TerminalColorScheme scheme);
    void apply_terminal_responses(const std::vector<TerminalResponseVariant>& responses);
    void apply_cell_size_responses(const std::vector<TerminalResponseVariant>& responses);

    Terminal& terminal_; // must outlive this host.
    std::unique_ptr<TerminalStreamDecoder> stream_decoder_;
    std::shared_ptr<RawInputListenerChain> raw_listeners_;
    std::vector<std::unique_ptr<Component>> children_;
    std::vector<std::unique_ptr<Component>> retired_children_;
    Component* focused_{nullptr};
    OverlayFocusRestoreState overlay_focus_restore_;
    std::size_t focus_order_counter_{0};
    std::shared_ptr<KeybindingsContext> keybinding_context_;
    bool color_scheme_notifications_enabled_{false};
    std::mutex scheme_listeners_mutex_;
    std::vector<std::pair<TerminalColorSchemeListenerId, TerminalColorSchemeListener>> scheme_listeners_;
    TerminalColorSchemeListenerId next_scheme_listener_id_{1};
    std::mutex color_queries_mutex_;
    std::deque<std::shared_ptr<QueryState>> pending_color_queries_;
    std::uint64_t next_query_id_{1};
};

} // namespace cch::tui::detail

#pragma once

// The Native TUI session synchronization adapter (#501 spec; extraction
// #505): owns the Agent Session event subscriptions, translates Agent
// lifecycle/streaming and session-assembly events into view updates, runs
// the turn auto-retry countdown (pi `CountdownTimer`), and computes the
// footer render inputs (pi footer.ts) from the bound session. The host
// wires running/view gates and presentation channels through
// SessionUiBindingHooks so the adapter never sees a Terminal or the
// interactive state.
//
// Repository-private `cch_coding_agent` implementation header: not part of
// an Owner Interface, not installed, never exported.

#include "coding_agent/AgentSession.hpp"
#include <cch/coding_agent/SessionProjectionSource.hpp>
#include "coding_agent/tui/Footer.hpp"
#include "coding_agent/tui/FooterDataProvider.hpp"

#include <cch/support/Error.hpp>

#include <boost/asio/any_io_executor.hpp>

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace cch::coding_agent::tui {

class InteractiveView;
class RetryCountdown;

/// Host operations used by SessionUiBinding. The binding owns subscription,
/// streaming-translation, retry-countdown, and footer-data mechanics; the
/// host supplies running/view gates and the non-modal presentation channels
/// without exposing the Terminal or the interactive state. Hooks capture the
/// host weakly; a null hook fails closed (the handler no-ops).
struct SessionUiBindingHooks {
    /// Whether the host run is live; event handlers no-op once it stops.
    std::move_only_function<bool()> is_live{nullptr};
    /// Resolve the composed main-screen view (null before composition).
    std::move_only_function<InteractiveView*()> view{nullptr};
    /// Whether an Agent prompt is in flight (pi's message_start working
    /// indicator gate).
    std::move_only_function<bool()> prompt_active{nullptr};
    /// Mark the current frame dirty (the host's TUI invalidate).
    std::move_only_function<void()> invalidate{nullptr};
    /// pi `showStatus`: one dim status line in the chat.
    std::move_only_function<void(std::string)> show_status{nullptr};
    /// pi `showError`: one diagnostic line in the chat.
    std::move_only_function<void(std::string)> show_error{nullptr};
    /// Boot path: the pending boot session's workspace while no session is
    /// bound (the footer renders it like pi's startup TUI).
    std::move_only_function<std::filesystem::path()> boot_workspace{nullptr};
    /// pi settings `compaction.enabled` for the footer's ` (auto)` marker;
    /// nullopt leaves the FooterData default.
    std::move_only_function<std::optional<bool>()> auto_compact_enabled{nullptr};
};

/// Session synchronization adapter (#505). One Agent Session is bound at a
/// time: `bind()` attaches both event sinks (pi `agent.subscribe` +
/// `session.on(...)`), and `detach()` releases them for session replacement
/// and final Close. Executor-confined like the host it serves; the footer
/// data computation is polled by the view's footer on every render.
class SessionUiBinding final : public SessionProjectionSource,
                               public std::enable_shared_from_this<SessionUiBinding> {
public:
    SessionUiBinding(
        boost::asio::any_io_executor executor,
        SessionUiBindingHooks hooks);
    SessionUiBinding(SessionUiBinding&&) = delete;
    SessionUiBinding& operator=(SessionUiBinding&&) = delete;
    ~SessionUiBinding() = default;
    SessionUiBinding(const SessionUiBinding&) = delete;
    SessionUiBinding& operator=(const SessionUiBinding&) = delete;

    /// Attach the Agent lifecycle/streaming and session-assembly
    /// (auto-retry, compaction) sinks to the session and re-baseline the
    /// displayed-diagnostics sync for the new binding. The session must
    /// outlive the binding or be detached first.
    [[nodiscard]] support::ExpectedVoid bind(AgentSession& session);

    /// Release both subscriptions (session replacement and final Close);
    /// idempotent.
    void detach() noexcept;

    /// pi `renderInitialMessages` diagnostic lines: render every snapshot
    /// Agent diagnostic and remember it as displayed so the incremental
    /// sync does not re-append it.
    void append_snapshot_diagnostics(
        const std::vector<support::Error>& diagnostics);

    /// Incremental sync of the pending-input queues and newly appeared Agent
    /// diagnostics into the view (after events and prompt completions).
    void sync_session_observations();

    /// Sync only the pending-input queue presentation.
    void sync_pending_input();

    /// pi footer.ts render inputs computed from the bound session snapshot
    /// and the model runtime (usage totals over the message history, the
    /// latest assistant cache hit rate, the context estimate, the model and
    /// thinking level, the subscription marker, and the available-provider
    /// count).
    [[nodiscard]] FooterData compute_footer_data();
    // ── SessionProjectionSource ──────────────────────────────────────────

    [[nodiscard]] uint64_t state_version() const noexcept override;

    [[nodiscard]] std::shared_ptr<const AgentSessionSnapshot> snapshot() const override;

    void set_dirty_listener(std::move_only_function<void()> on_dirty) override;

private:
    void on_event(const agent::AgentLifecycleEvent& event);
    void on_session_event(const AgentSessionEvent& event);

    /// pi `CountdownTimer` for the retry indicator: one-second ticks rewrite
    /// the `Retrying (n/m) in Ns...` message until the delay elapses.
    void start_retry_countdown(int attempt, int max_attempts, int seconds);
    void cancel_retry_countdown();
    void on_retry_tick(int attempt, int max_attempts, int remaining);

    [[nodiscard]] bool is_live();
    [[nodiscard]] InteractiveView* view();
    [[nodiscard]] bool prompt_active();

    boost::asio::any_io_executor executor_;
    SessionUiBindingHooks hooks_;
    AgentSession* session_{nullptr}; // must outlive the binding or be detached first.
    std::optional<EventSubscription> subscription_;
    /// Session-assembly event subscription (pi's `session.on(...)` for
    /// auto-retry and compaction events).
    std::optional<SessionEventSubscription> session_event_subscription_;
    /// The active retry countdown (pi `CountdownTimer`); null while no retry
    /// backoff is pending.
    std::shared_ptr<RetryCountdown> retry_countdown_;
    /// Git branch source for the footer's pwd line (pi
    /// `FooterDataProvider` subset).
    FooterDataProvider footer_data_provider_{std::filesystem::path{}};
    std::vector<std::string> displayed_agent_diagnostics_;
    mutable std::atomic<uint64_t> state_version_{1};
    mutable std::atomic<std::shared_ptr<const AgentSessionSnapshot>> fallback_snapshot_{nullptr};
    std::move_only_function<void()> dirty_listener_{nullptr};
};

} // namespace cch::coding_agent::tui

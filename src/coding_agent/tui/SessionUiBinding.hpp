#pragma once

// The Native TUI session synchronization adapter (#501 spec; extraction
// #505): owns the Agent Session Projection Stream subscription, translates
// the complete read model into view updates, runs the turn auto-retry
// countdown (pi `CountdownTimer`), and computes the footer render inputs
// (pi footer.ts) from the bound session. The host wires running/view gates
// and presentation channels through SessionUiBindingHooks so the adapter
// never sees a Terminal or the interactive state.
//
// Repository-private `cch_coding_agent` implementation header: not part of
// an Owner Interface, not installed, never exported.

#include "coding_agent/AgentSession.hpp"
#include <cch/coding_agent/ProjectionStream.hpp>
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
    /// Mark the current frame dirty (the host's TUI invalidate).
    std::move_only_function<void()> invalidate{nullptr};
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
/// time: `bind()` attaches only the Projection Stream subscription (ADR 0052)
/// and `detach()` releases it for session replacement and final Close.
/// Executor-confined like the host it serves; the footer data computation is
/// polled by the view's footer on every render.
///
/// The Projection Stream is the only business-state channel: the binding
/// composes its snapshot from the Base and every applied PatchMsg batch, and
/// the frame ticker drains the mailbox and reconciles from the composed value.
/// The two legacy event subscriptions remain notification-only: their
/// payloads are ignored and only request a projection drain/render, so
/// attach and mailbox-overflow resync have identical business state.
class SessionUiBinding final : public std::enable_shared_from_this<SessionUiBinding> {
public:
    SessionUiBinding(
        boost::asio::any_io_executor executor,
        SessionUiBindingHooks hooks);
    SessionUiBinding(SessionUiBinding&&) = delete;
    SessionUiBinding& operator=(SessionUiBinding&&) = delete;
    ~SessionUiBinding() = default;
    SessionUiBinding(const SessionUiBinding&) = delete;
    SessionUiBinding& operator=(const SessionUiBinding&) = delete;

    /// Attach the Projection Stream to the session and re-baseline the
    /// displayed-diagnostics sync for the new binding. The session must
    /// outlive the binding or be detached first.
    [[nodiscard]] support::ExpectedVoid bind(AgentSession& session);

    /// Release the projection subscription (session replacement and final
    /// Close); idempotent.
    void detach() noexcept;

    /// pi `renderInitialMessages` diagnostic lines: render every snapshot
    /// Agent diagnostic and remember it as displayed so the incremental
    /// sync does not re-append it.
    void append_snapshot_diagnostics(
        const std::vector<support::Error>& diagnostics);

    /// Drain and reconcile the composed projection at prompt completion,
    /// including pending-input queues, transcript, tools, statuses, and newly
    /// appeared diagnostics.
    void sync_session_observations();
    /// Replace the session-backed view state from the composed projection
    /// snapshot. The frame ticker calls this once before its render pass;
    /// status indicators, messages, tools, diagnostics, and queues come from
    /// the complete snapshot. Returns
    /// false when there is no live view to apply to so the ticker keeps the
    /// pending counted frame for the next frame (#597).
    [[nodiscard]] bool reconcile_snapshot(const AgentSessionSnapshot& snapshot);

    /// Sync only the pending-input queue presentation.
    void sync_pending_input();

    /// Drain the bound session's Projection Stream mailbox into the composed
    /// snapshot (ADR 0052 mailbox draining). Returns true when at least one
    /// stream message was applied; the frame ticker reconciles from the
    /// composed value and, on a counted (non-preview) frame render, clears
    /// the pending counted frame. Returns false when there is no live
    /// subscription to drain.
    [[nodiscard]] bool drain_composed_snapshot();

    /// The composed projection snapshot: the attach-time Base with every
    /// applied PatchMsg batch (ADR 0052 convergence invariant: it equals the
    /// Core snapshot at the last drained version).
    [[nodiscard]] const AgentSessionSnapshot& composed_snapshot() const noexcept { return composed_; }

    /// Whether stream messages were applied since the last counted frame
    /// render. The latency-first preview tier (#614) drains the mailbox but
    /// never consumes the counted frame: the ticker stays the authoritative
    /// counted render (ADR 0051/0052).
    [[nodiscard]] bool pending_counted_frame() const noexcept { return pending_counted_frame_; }
    void clear_pending_counted_frame() noexcept { pending_counted_frame_ = false; }

    /// pi footer.ts render inputs computed from the bound session snapshot
    /// and the model runtime (usage totals over the message history, the
    /// latest assistant cache hit rate, the context estimate, the model and
    /// thinking level, the subscription marker, and the available-provider
    /// count).
    [[nodiscard]] FooterData compute_footer_data();

private:
    enum class SessionStatus { Idle, Working, Retry, Compaction };

    /// One Projection Stream message arrived on the draining domain (the
    /// frame ticker): update the composed snapshot. The Base replaces it
    /// wholesale (attach and mailbox-overflow resync); a PatchMsg batch
    /// applies on top (ADR 0052).
    void on_projection_message(const ProjectionStreamMessageVariant& message);

    /// Request a frame after a Core publication. The event payload is
    /// intentionally ignored; business state is read only from the projection
    /// mailbox.
    void request_projection_frame();

    /// pi `CountdownTimer` for the retry indicator: one-second ticks rewrite
    /// the `Retrying (n/m) in Ns...` message until the delay elapses.
    void start_retry_countdown(int attempt, int max_attempts, int seconds);
    void cancel_retry_countdown();
    void on_retry_tick(int attempt, int max_attempts, int remaining);

    [[nodiscard]] bool is_live();
    [[nodiscard]] InteractiveView* view();

    boost::asio::any_io_executor executor_;
    SessionUiBindingHooks hooks_;
    AgentSession* session_{nullptr}; // must outlive the binding or be detached first.
    /// Notification-only subscriptions keep the projection mailbox responsive;
    /// they never read or translate their event payloads into business state.
    std::optional<EventSubscription> projection_activity_subscription_;
    std::optional<SessionEventSubscription> projection_recovery_subscription_;
    /// The active retry countdown (pi `CountdownTimer`); null while no retry
    /// backoff is pending.
    std::shared_ptr<RetryCountdown> retry_countdown_;
    /// Git branch source for the footer's pwd line (pi
    /// `FooterDataProvider` subset).
    FooterDataProvider footer_data_provider_{std::filesystem::path{}};
    std::vector<std::string> displayed_agent_diagnostics_;
    std::vector<std::string> displayed_session_event_diagnostics_;
    std::optional<std::string> displayed_run_error_;
    std::optional<std::string> displayed_retry_error_;
    std::optional<std::string> displayed_compaction_error_;
    CompactionState displayed_compaction_state_{CompactionState::Idle};
    SessionStatus session_status_{SessionStatus::Idle};
    /// The Projection Stream subscription (ADR 0052) and the snapshot the
    /// mailbox drains compose: the attach-time Base with every applied
    /// PatchMsg batch.
    ProjectionSubscription projection_subscription_{};
    AgentSessionSnapshot composed_{};
    /// Stream messages applied since the last counted frame render (the
    /// preview tier never consumes it; ADR 0051/0052).
    bool pending_counted_frame_{false};
};

} // namespace cch::coding_agent::tui

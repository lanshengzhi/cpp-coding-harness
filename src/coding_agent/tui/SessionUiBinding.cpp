#include "SessionUiBinding.hpp"

#include "coding_agent/tui/ErrorPresentation.hpp"
#include "coding_agent/tui/InteractiveView.hpp"

#include "agent/harness/compaction/Compaction.hpp"

#include <cch/ai/Message.hpp>

#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <set>
#include <type_traits>
#include <utility>
#include <variant>

namespace cch::coding_agent::tui {

/// pi `CountdownTimer`: the retry indicator's one-second countdown over the
/// backoff delay. Each tick delivers the remaining seconds; the timer stops
/// itself at zero. Executor-driven like the autocomplete debounce so the
/// countdown is observable on the interactive executor.
class RetryCountdown final : public std::enable_shared_from_this<RetryCountdown> {
public:
    RetryCountdown(
        boost::asio::any_io_executor executor,
        int attempt,
        int max_attempts)
        : executor_(std::move(executor)),
          attempt_(attempt),
          max_attempts_(max_attempts) {}

    /// Begin the countdown from `seconds` (pi's `remainingSeconds`), with
    /// the first tick one second later.
    void start(int seconds, std::move_only_function<void(int)> on_tick) {
        const auto self = shared_from_this();
        boost::asio::post(executor_, [self, seconds, on_tick = std::move(on_tick)]() mutable {
            self->remaining_ = std::max(0, seconds);
            self->on_tick_ = std::move(on_tick);
            self->schedule_tick();
        });
    }

    [[nodiscard]] int attempt() const { return attempt_; }
    [[nodiscard]] int max_attempts() const { return max_attempts_; }

    /// Stop the countdown: cancel the pending tick and drop the tick sink so
    /// neither keeps the countdown alive past its use (ASan, issue #473).
    /// Executor-confined like start().
    void cancel() {
        on_tick_ = nullptr;
        // Executor-confined; cancelling a live timer does not fail.
        (void)timer_.cancel();
    }

private:
    void schedule_tick() {
        const auto self = shared_from_this();
        timer_.expires_after(std::chrono::seconds(1));
        timer_.async_wait([self](const boost::system::error_code& error) {
            if (error || self->remaining_ <= 0) return;
            self->remaining_--;
            if (self->on_tick_) self->on_tick_(self->remaining_);
            if (self->remaining_ > 0) self->schedule_tick();
        });
    }

    boost::asio::any_io_executor executor_;
    boost::asio::steady_timer timer_{executor_};
    std::move_only_function<void(int)> on_tick_;
    int attempt_{0};
    int max_attempts_{0};
    int remaining_{0};
};

SessionUiBinding::SessionUiBinding(
    boost::asio::any_io_executor executor,
    SessionUiBindingHooks hooks)
    : executor_(std::move(executor)), hooks_(std::move(hooks)) {}

support::ExpectedVoid SessionUiBinding::bind(AgentSession& session) {
    if (session_ != nullptr) detach();
    session_ = &session;
    // The new session's diagnostics re-baseline: the incremental sync must
    // not re-append the diagnostics the bind-time initialize renders.
    displayed_agent_diagnostics_.clear();
    displayed_session_event_diagnostics_.clear();
    displayed_run_error_.reset();
    displayed_retry_error_.reset();
    displayed_compaction_error_.reset();
    displayed_compaction_state_ = CompactionState::Idle;
    session_status_ = SessionStatus::Idle;
    // ADR 0052: attach the Projection Stream subscription. The Base seeds
    // the composed snapshot immediately (drained here, executor-confined
    // like the Core's serialized domain); the frame ticker drains the rest.
    const auto weak = weak_from_this();
    projection_subscription_ = session.attach_projection([weak](const ProjectionStreamMessageVariant& message) {
        if (const auto self = weak.lock()) self->on_projection_message(message);
    });
    (void)projection_subscription_.drain();
    // The Base seeded the composed snapshot; the engine's bind flow renders
    // the initial view itself, so this drain consumes no counted frame.
    pending_counted_frame_ = false;

    // The pull stream owns all business-state translation. These weak event
    // subscriptions are only wakeups: they discard their payload and ask the
    // host to run the normal projection drain/render path. Keeping wakeups
    // separate from the state channel preserves low-latency status updates
    // without making event history part of attach or overflow recovery.
    if (auto subscribed = session.subscribe([weak](const agent::AgentLifecycleEvent&) -> support::ExpectedVoid {
            if (const auto self = weak.lock()) self->request_projection_frame();
            return {};
        });
            !subscribed) {
        detach();
        return std::unexpected(subscribed.error());
    } else {
        projection_activity_subscription_.emplace(std::move(*subscribed));
    }
    if (auto subscribed = session.subscribe_session([weak](const AgentSessionEvent&) -> support::ExpectedVoid {
            if (const auto self = weak.lock()) self->request_projection_frame();
            return {};
        });
            !subscribed) {
        detach();
        return std::unexpected(subscribed.error());
    } else {
        projection_recovery_subscription_.emplace(std::move(*subscribed));
    }
    return {};
}

void SessionUiBinding::detach() noexcept {
    projection_subscription_.unsubscribe();
    projection_activity_subscription_.reset();
    projection_recovery_subscription_.reset();
    composed_ = AgentSessionSnapshot{};
    pending_counted_frame_ = false;
    cancel_retry_countdown();
    displayed_run_error_.reset();
    displayed_retry_error_.reset();
    displayed_compaction_error_.reset();
    displayed_compaction_state_ = CompactionState::Idle;
    session_ = nullptr;
    session_status_ = SessionStatus::Idle;
}

void SessionUiBinding::append_snapshot_diagnostics(
    const std::vector<support::Error>& diagnostics) {
    auto* const active_view = view();
    if (active_view == nullptr) return;
    for (const auto& diagnostic : diagnostics) {
        auto text = combined_error_text(diagnostic);
        active_view->append_diagnostic(text);
        displayed_agent_diagnostics_.push_back(std::move(text));
    }
}

void SessionUiBinding::sync_pending_input() {
    if (!is_live()) return;
    auto* const active_view = view();
    if (active_view == nullptr || !projection_subscription_) return;
    (void)drain_composed_snapshot();
    active_view->set_pending_input(composed_.agent_state.input_queues);
}

void SessionUiBinding::sync_session_observations() {
    if (!is_live() || view() == nullptr || !projection_subscription_) return;
    const bool dirty = drain_composed_snapshot();
    // Prompt completion is an immediate observation boundary. Reconcile here
    // as well as from the frame ticker so a settled answer is visible before
    // the caller's completion callback returns; the ticker remains the normal
    // path for streaming updates.
    (void)reconcile_snapshot(composed_);
    if (dirty && hooks_.invalidate != nullptr) hooks_.invalidate();
}

void SessionUiBinding::on_projection_message(const ProjectionStreamMessageVariant& message) {
    std::visit(
            [this](const auto& value) {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<T, ProjectionStreamBase>) {
                    // Attach and mailbox-overflow resync: the Base replaces
                    // the composed value wholesale (ADR 0052).
                    composed_ = value.snapshot;
                } else {
                    apply_projection_patches(composed_, value.patches);
                }
            },
            message);
    pending_counted_frame_ = true;
}

void SessionUiBinding::request_projection_frame() {
    if (session_ == nullptr || !is_live() || hooks_.invalidate == nullptr) return;
    hooks_.invalidate();
}

bool SessionUiBinding::drain_composed_snapshot() {
    if (!projection_subscription_) {
        return false;
    }
    return projection_subscription_.drain() > 0;
}

bool SessionUiBinding::reconcile_snapshot(const AgentSessionSnapshot& snapshot) {
    if (!is_live()) return false;
    auto* const active_view = view();
    if (active_view == nullptr) return false;

    active_view->reconcile_snapshot(snapshot);
    active_view->set_pending_input(snapshot.agent_state.input_queues);

    const auto append_new_diagnostics = [&active_view](const auto& diagnostics, auto& displayed) {
        std::vector<std::string> current;
        current.reserve(diagnostics.size());
        for (const auto& diagnostic : diagnostics) {
            current.push_back(combined_error_text(diagnostic));
        }
        auto overlap = std::min(displayed.size(), current.size());
        while (overlap > 0 &&
                !std::equal(displayed.end() - static_cast<std::ptrdiff_t>(overlap), displayed.end(), current.begin())) {
            --overlap;
        }
        for (auto index = overlap; index < current.size(); ++index) {
            active_view->append_diagnostic(current[index]);
        }
        displayed = std::move(current);
    };
    append_new_diagnostics(snapshot.agent_state.diagnostics, displayed_agent_diagnostics_);
    append_new_diagnostics(snapshot.session_event_diagnostics, displayed_session_event_diagnostics_);

    // A successful compaction replaces the transcript rather than appending
    // to it. Its terminal state is projection-backed, so rebuild only on the
    // Running -> Succeeded transition and let the normal reconciliation path
    // preserve the same snapshot-owned business state thereafter.
    const auto compaction_state = snapshot.recovery_state.compaction;
    if (compaction_state != displayed_compaction_state_ && compaction_state == CompactionState::Succeeded &&
            displayed_compaction_state_ == CompactionState::Running) {
        active_view->initialize(snapshot);
    }
    displayed_compaction_state_ = compaction_state;

    // Run and recovery status is read-model state. The countdown is only
    // created when the phase changes into retry, so each later frame observes
    // the same deadline without resetting the timer.
    if (snapshot.run_state.phase == RunPhase::Running) {
        if (session_status_ != SessionStatus::Working) {
            cancel_retry_countdown();
            session_status_ = SessionStatus::Working;
            active_view->show_status_working();
        }
    } else if (snapshot.run_state.phase == RunPhase::Retrying) {
        const auto attempt = static_cast<int>(snapshot.recovery_state.retry_count);
        const auto max_attempts = static_cast<int>(snapshot.recovery_state.max_retry_count);
        auto seconds = 1;
        if (snapshot.recovery_state.next_retry_at_ms) {
            const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                                     .count();
            const auto remaining = std::max<std::int64_t>(0, *snapshot.recovery_state.next_retry_at_ms - now);
            seconds = static_cast<int>(std::max<std::int64_t>(1, (remaining + 999) / 1000));
        }
        if (session_status_ != SessionStatus::Retry || !retry_countdown_) {
            cancel_retry_countdown();
            session_status_ = SessionStatus::Retry;
            active_view->show_status_retry(attempt, max_attempts, seconds);
            start_retry_countdown(attempt, max_attempts, seconds);
        }
    } else if (snapshot.run_state.phase == RunPhase::Compacting) {
        if (session_status_ != SessionStatus::Compaction) {
            cancel_retry_countdown();
            session_status_ = SessionStatus::Compaction;
            active_view->show_status_compaction(snapshot.recovery_state.compaction_reason.value_or("manual"));
        }
    } else if (session_status_ != SessionStatus::Idle) {
        cancel_retry_countdown();
        session_status_ = SessionStatus::Idle;
        active_view->clear_status_indicator();
    }

    if (snapshot.run_state.error != displayed_run_error_) {
        if (snapshot.run_state.error && hooks_.show_error != nullptr) {
            hooks_.show_error(*snapshot.run_state.error);
        }
        displayed_run_error_ = snapshot.run_state.error;
    }
    const auto retry_error = snapshot.run_state.terminal == RunTerminalState::Failed
                                     ? snapshot.recovery_state.retry_error
                                     : std::nullopt;
    if (retry_error != displayed_retry_error_) {
        if (retry_error && hooks_.show_error != nullptr) {
            hooks_.show_error(std::format(
                    "Retry failed after {} attempts: {}", snapshot.recovery_state.retry_count, *retry_error));
        }
        displayed_retry_error_ = retry_error;
    }
    if (snapshot.recovery_state.compaction_error != displayed_compaction_error_) {
        if (snapshot.recovery_state.compaction_error) {
            const auto reason = snapshot.recovery_state.compaction_reason.value_or("manual");
            if (reason == "manual" && hooks_.show_error != nullptr) {
                hooks_.show_error(*snapshot.recovery_state.compaction_error);
            } else {
                active_view->append_diagnostic(*snapshot.recovery_state.compaction_error);
            }
        }
        displayed_compaction_error_ = snapshot.recovery_state.compaction_error;
    }
    return true;
}

/// pi `CountdownTimer` for the retry indicator: one-second ticks rewrite
/// the `Retrying (n/m) in Ns...` message until the delay elapses. The
/// tick captures the attempt values (never the countdown itself) so the
/// countdown's stored `on_tick_` cannot form a self-cycle that leaks the
/// binding (ASan, issue #473).
void SessionUiBinding::start_retry_countdown(int attempt, int max_attempts, int seconds) {
    auto countdown = std::make_shared<RetryCountdown>(
        executor_, attempt, max_attempts);
    retry_countdown_ = countdown;
    countdown->start(seconds, [weak = weak_from_this(), attempt, max_attempts](int remaining) {
        if (const auto self = weak.lock()) {
            self->on_retry_tick(attempt, max_attempts, remaining);
        }
    });
}

void SessionUiBinding::cancel_retry_countdown() {
    if (retry_countdown_) retry_countdown_->cancel();
    retry_countdown_.reset();
}

void SessionUiBinding::on_retry_tick(int attempt, int max_attempts, int remaining) {
    if (!is_live()) return;
    auto* const active_view = view();
    if (active_view == nullptr) return;
    active_view->set_status_retry_message(attempt, max_attempts, remaining);
    if (hooks_.invalidate != nullptr) hooks_.invalidate();
}

FooterData SessionUiBinding::compute_footer_data() {
    if (session_ == nullptr) {
        // Boot path: the main screen renders while the boot trust prompt
        // overlay is up, before the session binds; the footer shows the
        // boot workspace like pi's startup TUI.
        FooterData data;
        data.cwd = hooks_.boot_workspace != nullptr
            ? hooks_.boot_workspace()
            : std::filesystem::path{};
        return data;
    }
    FooterData data;
    const auto& snapshot = composed_;
    data.cwd = session_->workspace();
    footer_data_provider_.set_cwd(data.cwd);
    data.git_branch = footer_data_provider_.git_branch();

    // Usage totals: every assistant message's usage accumulates (pi
    // `addUsageToTotals` over the session entries; the C++ message
    // history is the in-memory entry equivalent, and toolResult/
    // compaction usage is not carried on the C++ message values).
    for (const auto& message : snapshot.agent_state.messages) {
        const auto* assistant = std::get_if<ai::AssistantMessage>(&message);
        if (assistant == nullptr) continue;
        data.input += assistant->usage.input;
        data.output += assistant->usage.output;
        data.cache_read += assistant->usage.cache_read;
        data.cache_write += assistant->usage.cache_write;
        data.cost += assistant->usage.cost.total;
        // pi keeps the latest assistant message's prompt hit rate.
        const auto prompt_tokens =
            assistant->usage.input +
            assistant->usage.cache_read +
            assistant->usage.cache_write;
        if (prompt_tokens > 0) {
            data.cache_hit_rate =
                (static_cast<double>(assistant->usage.cache_read) /
                 static_cast<double>(prompt_tokens)) * 100.0;
        } else {
            data.cache_hit_rate.reset();
        }
    }

    // Context usage (pi `getContextUsage` subset): the model's context
    // window with the estimated tokens; after a compaction, tokens are
    // unknown until a valid assistant usage lands after the boundary.
    const auto& model = snapshot.agent_state.model;
    data.context_window = static_cast<std::size_t>(model.context_window);
    data.model_id = model.id;
    data.provider = model.provider;
    data.model_reasoning = model.reasoning;
    data.thinking_level = snapshot.agent_state.thinking_level.empty()
        ? std::string{"off"}
        : snapshot.agent_state.thinking_level;
    if (data.context_window > 0) {
        const auto& messages = snapshot.agent_state.messages;
        std::optional<std::size_t> latest_compaction;
        for (std::size_t index = 0; index < messages.size(); ++index) {
            if (std::holds_alternative<ai::CompactionSummaryMessage>(messages[index])) {
                latest_compaction = index;
            }
        }
        bool post_compaction_usage = false;
        if (latest_compaction) {
            for (std::size_t index = *latest_compaction + 1;
                 index < messages.size();
                 ++index) {
                const auto* assistant =
                    std::get_if<ai::AssistantMessage>(&messages[index]);
                if (assistant == nullptr) continue;
                if (assistant->stop_reason != ai::AssistantStopReason::Aborted &&
                    assistant->stop_reason != ai::AssistantStopReason::Error &&
                    harness::session::calculate_context_tokens(assistant->usage) > 0) {
                    post_compaction_usage = true;
                    break;
                }
            }
            if (!post_compaction_usage) {
                data.context_tokens = std::nullopt;
            }
        }
        if (!latest_compaction || post_compaction_usage) {
            data.context_tokens =
                harness::session::estimate_context_tokens(messages).tokens;
        }
    }

    // pi `usingSubscription`: kimi-coding, or any provider authenticating
    // through an OAuth credential. The runtime may be absent on
    // focused-test sessions; both markers stay off then.
    const auto runtime = session_->model_runtime();
    if (!model.id.empty() && runtime) {
        data.using_subscription =
            model.provider == "kimi-coding" ||
            runtime->is_using_oauth(model.provider);
    }

    // pi `updateAvailableProviderCount`: unique providers in the scoped
    // set, or in the runtime's availability snapshot.
    const auto& scoped = session_->scoped_models();
    std::set<std::string> providers;
    if (!scoped.empty()) {
        for (const auto& entry : scoped) providers.insert(entry.model.provider);
    } else if (runtime) {
        for (const auto& available : runtime->get_available_snapshot()) {
            providers.insert(available.provider);
        }
    }
    data.available_provider_count = providers.size();

    if (hooks_.auto_compact_enabled != nullptr) {
        if (const auto enabled = hooks_.auto_compact_enabled(); enabled) {
            data.auto_compact_enabled = *enabled;
        }
    }
    return data;
}

bool SessionUiBinding::is_live() {
    return hooks_.is_live != nullptr && hooks_.is_live();
}

InteractiveView* SessionUiBinding::view() {
    return hooks_.view != nullptr ? hooks_.view() : nullptr;
}

} // namespace cch::coding_agent::tui

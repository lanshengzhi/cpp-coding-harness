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
    session_ = &session;
    // The new session's diagnostics re-baseline: the incremental sync must
    // not re-append the diagnostics the bind-time initialize renders.
    displayed_agent_diagnostics_.clear();
    const auto weak = weak_from_this();
    if (auto subscribed = session.subscribe(
            [weak](const agent::AgentLifecycleEvent& event) -> support::ExpectedVoid {
                if (const auto self = weak.lock()) self->on_event(event);
                return {};
            });
        !subscribed) {
        return std::unexpected(subscribed.error());
    } else {
        subscription_.emplace(std::move(*subscribed));
    }
    if (auto subscribed = session.subscribe_session(
            [weak](const AgentSessionEvent& event) -> support::ExpectedVoid {
                if (const auto self = weak.lock()) self->on_session_event(event);
                return {};
            });
        !subscribed) {
        return std::unexpected(subscribed.error());
    } else {
        session_event_subscription_.emplace(std::move(*subscribed));
    }
    return {};
}

void SessionUiBinding::detach() noexcept {
    subscription_.reset();
    session_event_subscription_.reset();
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
    if (active_view == nullptr || session_ == nullptr || !session_->is_open()) return;
    active_view->set_pending_input(session_->snapshot().agent_state.input_queues);
}

void SessionUiBinding::sync_session_observations() {
    if (!is_live()) return;
    auto* const active_view = view();
    if (active_view == nullptr || session_ == nullptr || !session_->is_open()) return;
    const auto snapshot = session_->snapshot();
    active_view->set_pending_input(snapshot.agent_state.input_queues);

    std::vector<std::string> current;
    current.reserve(snapshot.agent_state.diagnostics.size());
    for (const auto& diagnostic : snapshot.agent_state.diagnostics) {
        current.push_back(combined_error_text(diagnostic));
    }

    auto overlap = std::min(displayed_agent_diagnostics_.size(), current.size());
    while (overlap > 0 && !std::equal(
            displayed_agent_diagnostics_.end() - static_cast<std::ptrdiff_t>(overlap),
            displayed_agent_diagnostics_.end(),
            current.begin())) {
        --overlap;
    }
    for (auto index = overlap; index < current.size(); ++index) {
        active_view->append_diagnostic(current[index]);
    }
    displayed_agent_diagnostics_ = std::move(current);
}

void SessionUiBinding::on_event(const agent::AgentLifecycleEvent& /*event*/) {
    if (!is_live()) return;
    if (session_ != nullptr) {
        session_->update_projection();
    } else {
        state_version_.fetch_add(1, std::memory_order_release);
        if (dirty_listener_) {
            dirty_listener_();
        }
    }
}

/// pi's `session.on("auto_retry_start"...` / `compaction_start...`
/// handlers: the Retry indicator with the backoff countdown, the
/// Compaction indicator with the reason wording, and the end-event
/// cleanup with pi's statuses.
void SessionUiBinding::on_session_event(const AgentSessionEvent& event) {
    if (!is_live()) return;
    auto* const active_view = view();
    if (active_view == nullptr) return;
    if (const auto* retry = std::get_if<AutoRetryStartEvent>(&event)) {
        cancel_retry_countdown();
        const auto seconds = static_cast<int>(std::max<std::int64_t>(
            1, (retry->delay_ms + 999) / 1000));
        active_view->show_status_retry(
            retry->attempt, retry->max_attempts, seconds);
        start_retry_countdown(retry->attempt, retry->max_attempts, seconds);
    } else if (const auto* retry_end = std::get_if<AutoRetryEndEvent>(&event)) {
        cancel_retry_countdown();
        active_view->clear_status_indicator();
        // pi auto_retry_end: only the final failure reports (success
        // shows the ordinary response).
        if (!retry_end->success && hooks_.show_error != nullptr) {
            hooks_.show_error(std::format(
                "Retry failed after {} attempts: {}",
                retry_end->attempt,
                retry_end->final_error.value_or("Unknown error")));
        }
    } else if (const auto* compaction = std::get_if<CompactionStartEvent>(&event)) {
        active_view->show_status_compaction(compaction->reason);
    } else if (const auto* compaction_end = std::get_if<CompactionEndEvent>(&event)) {
        active_view->clear_status_indicator();
        if (compaction_end->aborted) {
            if (compaction_end->reason == "manual") {
                if (hooks_.show_error != nullptr) {
                    hooks_.show_error("Compaction cancelled");
                }
            } else if (hooks_.show_status != nullptr) {
                hooks_.show_status("Auto-compaction cancelled");
            }
        } else if (compaction_end->error_message) {
            if (compaction_end->reason == "manual") {
                if (hooks_.show_error != nullptr) {
                    hooks_.show_error(*compaction_end->error_message);
                }
            } else {
                active_view->append_diagnostic(*compaction_end->error_message);
            }
        } else {
            // pi compaction_end with a result: rebuild the chat from the
            // fresh snapshot (the compaction summary renders as the
            // latest entry) and refresh the footer's usage totals.
            const auto snapshot = session_->snapshot();
            active_view->initialize(snapshot);
            active_view->set_pending_input(snapshot.agent_state.input_queues);
        }
    }
    if (hooks_.invalidate != nullptr) hooks_.invalidate();
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
    const auto snapshot = session_->snapshot();
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

bool SessionUiBinding::prompt_active() {
    return hooks_.prompt_active != nullptr && hooks_.prompt_active();
}
uint64_t SessionUiBinding::state_version() const noexcept {
    return session_ ? session_->state_version() : state_version_.load(std::memory_order_acquire);
}

std::shared_ptr<const AgentSessionSnapshot> SessionUiBinding::snapshot() const {
    if (session_ != nullptr) {
        return session_->projection_snapshot();
    }
    return fallback_snapshot_.load(std::memory_order_acquire);
}

void SessionUiBinding::set_dirty_listener(std::move_only_function<void()> on_dirty) {
    if (session_ != nullptr) {
        session_->set_dirty_listener(std::move(on_dirty));
    } else {
        dirty_listener_ = std::move(on_dirty);
    }
}

} // namespace cch::coding_agent::tui

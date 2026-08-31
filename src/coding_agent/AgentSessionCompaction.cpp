#include "coding_agent/AgentSessionImpl.hpp"

#include "agent/AgentMessageAccess.hpp"
#include "support/AsyncResultBridge.hpp"
#include "agent/harness/compaction/Compaction.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <exception>
#include <optional>
#include <utility>

namespace cch::coding_agent {

boost::asio::awaitable<support::Expected<CompactionResult>> AgentSession::Impl::compact(
        std::string custom_instructions) {
    if (auto rejected = reject_if_closed(); !rejected) {
        co_return std::unexpected(rejected.error());
    }
    if (compaction_active_) {
        co_return std::unexpected(
                support::make_error(support::ErrorCode::Validation, "compaction is already in flight"));
    }
    if (reload_active_) {
        co_return std::unexpected(support::make_error(support::ErrorCode::Busy, "session reload is already in flight"));
    }
    // Claim the in-flight guard before any await so concurrent compact()
    // calls reject here instead of interleaving at the summarization await.
    compaction_active_ = true;

    // pi `AgentSession.compact`: emit `compaction_start` before the run is
    // aborted so the Compaction status indicator shows for the whole flow.
    emit_session_event(CompactionStartEvent{.reason = "manual"});

    // pi AgentSession.compact: abort the active run first, then wait for it
    // to settle before compacting (the run settles with the ordinary aborted
    // terminal; its assistant message is already committed to the store).
    if (prompt_active_ && active_stop_source_) {
        (void)active_stop_source_->request_stop();
        if (prompt_settled_signal_) {
            boost::system::error_code wait_error;
            co_await prompt_settled_signal_->async_wait(
                    boost::asio::redirect_error(boost::asio::use_awaitable, wait_error));
        }
    }

    support::Expected<CompactionResult> result;
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
    try {
#endif
        result = co_await compact_impl(std::move(custom_instructions));
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
    } catch (const std::exception& error) {
        result = std::unexpected(
                support::make_error(support::ErrorCode::Unknown, "session compact coroutine failed", error.what()));
    } catch (...) {
        result = std::unexpected(support::make_error(support::ErrorCode::Unknown, "session compact coroutine failed"));
    }
#endif
    compaction_active_ = false;
    emit_session_event(CompactionEndEvent{
            .reason = "manual",
            .aborted = false,
            .error_message = result ? std::nullopt : std::optional<std::string>{result.error().message},
    });
    // A Close requested while the compaction was in flight finalizes here,
    // after the compaction reached its terminal outcome — never while the
    // compaction could still touch the Agent, the store, or the persistence
    // channel (issue #467).
    if (lifecycle_ == Lifecycle::Closing && !prompt_active_ && !user_bash_active_ && !reload_active_) {
        co_await finalize_close_after_active_work();
    }
    co_return result;
}

namespace {

[[nodiscard]] support::Error no_model_selected_error() {
    // pi formatNoModelSelectedMessage's login-help tail is Native TUI
    // presentation (auth-guidance); the C++ session carries only the core
    // directive. The placeholder kDefaultModel is the C++ "no model" state.
    return support::make_error(
            support::ErrorCode::Validation, "No model selected.\n\nThen use /model to select a model.");
}

} // namespace

boost::asio::awaitable<support::Expected<CompactionResult>> AgentSession::Impl::compact_impl(
        std::string custom_instructions) {
    if (!agent_ || !session_.store) {
        co_return std::unexpected(support::make_error(support::ErrorCode::Validation, "session Agent is unavailable"));
    }
    const auto model = agent_->state().model;
    if (model.id == agent::detail::kDefaultModel.id) {
        co_return std::unexpected(no_model_selected_error());
    }
    const auto store_path = session_.store ? session_.store->path() : std::nullopt;
    if (!store_path) {
        // In-memory sessions have no tree/entry surface: there is no session
        // file to persist a CompactionEntry into or to rebuild context from.
        co_return std::unexpected(
                support::make_error(support::ErrorCode::Validation, "compaction requires a persisted session file"));
    }
    auto outcome = co_await attempt_compaction(std::move(custom_instructions), {});
    if (!outcome) {
        co_return std::unexpected(outcome.error());
    }
    if (const auto* skipped = std::get_if<harness::session::CompactionSkipped>(&*outcome)) {
        // pi `AgentSession.compact`'s verbatim refusals, mapped from the
        // door's typed skip reason (pi re-inspected the branch tail).
        if (skipped->reason == harness::session::CompactionSkipped::Reason::AlreadyCompacted) {
            co_return std::unexpected(support::make_error(support::ErrorCode::Validation, "Already compacted"));
        }
        co_return std::unexpected(
                support::make_error(support::ErrorCode::Validation, "Nothing to compact (session too small)"));
    }
    co_return co_await commit_compaction(std::move(std::get<harness::session::CompactionResult>(*outcome)));
}

boost::asio::awaitable<support::Expected<harness::session::CompactionOutcomeVariant>>
AgentSession::Impl::attempt_compaction(
        std::string custom_instructions, harness::session::CompactionStartHook on_compaction_start) {
    if (!agent_ || !session_.store) {
        co_return std::unexpected(support::make_error(support::ErrorCode::Validation, "session Agent is unavailable"));
    }
    const auto store_path = session_.store->path();
    if (!store_path) {
        // In-memory sessions have no tree/entry surface: there is no session
        // file to persist a CompactionEntry into or to rebuild context from.
        co_return std::unexpected(
                support::make_error(support::ErrorCode::Validation, "compaction requires a persisted session file"));
    }
    const auto model = agent_->state().model;

    harness::session::SummarizationStreamFn summarization_stream =
            [factory = make_stream_factory(), model](ai::AiContext context, ai::SimpleStreamOptions options) mutable
            -> boost::asio::awaitable<support::Expected<ai::AssistantMessage>> {
        auto stream = factory(model, std::move(context), std::move(options));
        co_return co_await cch::support::detail::await_async_result(std::move(stream).run({}));
    };

    harness::session::CompactionRunOptions run_options;
    run_options.settings = effective_compaction_settings();
    if (!custom_instructions.empty()) {
        run_options.custom_instructions = std::move(custom_instructions);
    }
    run_options.thinking_level = agent_->state().thinking_level;
    run_options.stop_token = std::stop_token{};
    run_options.summarization_stream = std::move(summarization_stream);
    run_options.on_compaction_start = std::move(on_compaction_start);

    co_return co_await harness::session::compact(*session_.store, model, std::move(run_options));
}

boost::asio::awaitable<support::Expected<CompactionResult>> AgentSession::Impl::commit_compaction(
        harness::session::CompactionResult result) {
    if (!agent_ || !session_.store) {
        co_return std::unexpected(support::make_error(support::ErrorCode::Validation, "session Agent is unavailable"));
    }

    CompactionResult compaction_result;
    compaction_result.summary = result.summary;
    compaction_result.first_kept_entry_id = result.first_kept_entry_id;
    compaction_result.tokens_before = result.tokens_before;
    compaction_result.usage = result.usage;
    compaction_result.details = result.details;

    // Persist the CompactionEntry with pi's full field set (summary,
    // firstKeptEntryId, tokensBefore, retainedTail, details, usage; fromHook
    // false for machinery-generated compactions).
    // The CompactionEntry must land after the run's message entries: drain
    // the persistence channel before this mid-run typed append (ADR 0040).
    if (persistence_) {
        co_await persistence_->drain();
    }
    if (auto appended = session_.store->append_compaction(std::nullopt,
                harness::session::CompactionEntryValue{
                        .summary = std::move(result.summary),
                        .first_kept_entry_id = std::move(result.first_kept_entry_id),
                        .tokens_before = result.tokens_before,
                        .retained_tail = std::move(result.retained_tail),
                        .details = std::move(result.details),
                        .usage = result.usage,
                        .from_hook = false,
                });
            !appended) {
        co_return std::unexpected(appended.error());
    }

    // Rebuild the live context from the store's live tree exactly like pi
    // (`this.agent.state.messages = sessionContext.messages`): the next
    // prompt's model context is compactionSummary + retained tail. The
    // compaction append above already advanced the cached tree, so there is
    // nothing to re-read.
    auto context = session_.store->build_context();
    if (auto replaced = agent::detail::AgentMessageAccess::replace_messages(*agent_, context.messages); !replaced) {
        co_return std::unexpected(replaced.error());
    }

    std::size_t estimated_after = 0;
    for (const auto& message : context.messages) {
        estimated_after += harness::session::estimate_tokens(message);
    }
    compaction_result.estimated_tokens_after = estimated_after;
    co_return compaction_result;
}

boost::asio::awaitable<bool> AgentSession::Impl::run_auto_compaction(bool will_retry, std::string reason) {
    if (!agent_ || !session_.store) {
        co_return false;
    }
    const auto model = agent_->state().model;
    if (model.id == agent::detail::kDefaultModel.id) {
        co_return false;
    }
    const auto store_path = session_.store->path();
    if (!store_path) {
        // In-memory sessions have no tree/entry surface (same confinement as
        // the manual trigger): auto-compaction is skipped silently.
        co_return false;
    }

    // The triggering turn's terminal commitment is submitted synchronously
    // but settles through the persistence channel's Runtime hops, so the
    // branch read inside the door can race the final append and cut the
    // just-finished assistant answer out of the retained tail (issue #526).
    // Drain the channel first: event-driven quiescence, not a wall-clock wait.
    if (persistence_) {
        co_await persistence_->drain();
    }

    // pi `_runAutoCompaction`: the auto trigger emits `compaction_start`
    // with its reason after a successful preparation, before summarizing; a
    // not-applicable preparation returns false without emitting any events.
    harness::session::CompactionStartHook on_compaction_start = [this, reason] {
        emit_session_event(CompactionStartEvent{.reason = reason});
    };
    auto outcome = co_await attempt_compaction({}, std::move(on_compaction_start));
    if (!outcome) {
        emit_session_event(CompactionEndEvent{
                .reason = reason,
                .aborted = false,
                .error_message = outcome.error().message,
        });
        co_return false;
    }
    if (std::holds_alternative<harness::session::CompactionSkipped>(*outcome)) {
        co_return false;
    }
    auto result = co_await commit_compaction(std::move(std::get<harness::session::CompactionResult>(*outcome)));
    if (!result) {
        emit_session_event(CompactionEndEvent{
                .reason = reason,
                .aborted = false,
                .error_message = result.error().message,
        });
        co_return false;
    }

    if (will_retry) {
        // The rebuilt context can still end in the failed error assistant
        // message (it lives in the retained tail); drop it so the
        // continuation's last message is the user prompt (pi
        // `_runAutoCompaction` willRetry branch). The end event fires after
        // the drop so a chat rebuild on `compaction_end` never renders the
        // stale error.
        if (auto popped = agent::detail::AgentMessageAccess::pop_trailing_assistant(*agent_); !popped) {
            emit_session_event(CompactionEndEvent{
                    .reason = reason,
                    .aborted = false,
                    .error_message = popped.error().message,
            });
            co_return false;
        }
    }
    emit_session_event(CompactionEndEvent{
            .reason = std::move(reason),
            .aborted = false,
            .error_message = std::nullopt,
    });
    co_return will_retry;
}

boost::asio::awaitable<AgentSession::Impl::AutoCompactionOutcome> AgentSession::Impl::check_auto_compaction(
        const ai::AssistantMessage& assistant_message, bool skip_aborted_check) {
    const auto settings = effective_compaction_settings();
    if (!settings.enabled) {
        co_return AutoCompactionOutcome::None;
    }
    if (skip_aborted_check && assistant_message.stop_reason == ai::AssistantStopReason::Aborted) {
        co_return AutoCompactionOutcome::None;
    }

    const auto& model = agent_->state().model;
    const std::size_t context_window = static_cast<std::size_t>(model.context_window);
    const bool same_model = model.id == assistant_message.model && model.provider == assistant_message.provider;

    // Skip compaction checks when the assistant message predates the latest
    // compaction boundary: a stale pre-compaction usage/error must not
    // retrigger compaction on the first prompt after compaction (pi
    // `_checkCompaction` `assistantIsFromBeforeCompaction`).
    const auto compaction_timestamp = latest_compaction_timestamp();
    if (compaction_timestamp && assistant_message.timestamp <= *compaction_timestamp) {
        co_return AutoCompactionOutcome::None;
    }

    // Case 1: Overflow. An error terminal (or a successful response whose
    // usage already exceeds the window) compacts; only the error terminal
    // retries, because continue() cannot continue from a completed assistant
    // message. Overflow never routes to turn auto-retry: the post-run loop
    // excludes it via `is_retryable_error` before reaching this branch, so
    // the two recovery paths never interfere (T12's boundary).
    if (same_model && harness::session::is_context_overflow(assistant_message, context_window)) {
        const bool will_retry = assistant_message.stop_reason != ai::AssistantStopReason::Stop;
        if (!will_retry) {
            if (co_await run_auto_compaction(false, "overflow")) {
                co_return AutoCompactionOutcome::Compacted;
            }
            co_return AutoCompactionOutcome::None;
        }
        if (overflow_recovery_attempted_) {
            co_return AutoCompactionOutcome::OverflowRecoveryFailed;
        }
        overflow_recovery_attempted_ = true;
        // The overflow error message is saved to session history but must not
        // be re-sent to the model on the retry (pi removes it from agent
        // state before compacting).
        if (auto popped = agent::detail::AgentMessageAccess::pop_trailing_assistant(*agent_); !popped) {
            co_return AutoCompactionOutcome::None;
        }
        if (co_await run_auto_compaction(true, "overflow")) {
            co_return AutoCompactionOutcome::OverflowRetry;
        }
        co_return AutoCompactionOutcome::None;
    }

    // Case 2: Threshold — `contextTokens > contextWindow - reserveTokens`
    // compacts with no retry. For error messages or all-zero usage, estimate
    // from the last valid response so persistent API errors still compact.
    // A model without a known context window (0) has no threshold to compact
    // against: pi's catalog models always carry a window, while the C++
    // placeholders (kDefaultModel and the test sentinel) carry none, so this
    // is a recorded C++ divergence that keeps unknown-window sessions from
    // compacting on every turn (error-based overflow still fires above,
    // independent of the window, exactly like pi's `isContextOverflow`).
    if (context_window == 0) {
        co_return AutoCompactionOutcome::None;
    }
    const std::size_t direct_context_tokens = harness::session::calculate_context_tokens(assistant_message.usage);
    std::size_t context_tokens = direct_context_tokens;
    if (assistant_message.stop_reason == ai::AssistantStopReason::Error || direct_context_tokens == 0) {
        const auto estimate = harness::session::estimate_context_tokens(agent_->state().messages);
        if (!estimate.last_usage_index) {
            // No usage data at all: nothing to base a threshold decision on.
            co_return AutoCompactionOutcome::None;
        }
        if (compaction_timestamp) {
            // The usage source must be post-compaction: kept pre-compaction
            // messages carry stale usage reflecting the old (larger) context
            // and would falsely trigger compaction right after one finished.
            // state() returns a by-value snapshot; bind a copy or the
            // reference would dangle into the temporary's message vector.
            const auto usage_message = agent_->state().messages[*estimate.last_usage_index];
            const auto* usage_assistant = std::get_if<ai::AssistantMessage>(&usage_message);
            if (usage_assistant != nullptr && usage_assistant->timestamp <= *compaction_timestamp) {
                co_return AutoCompactionOutcome::None;
            }
        }
        context_tokens = estimate.tokens;
    }
    if (harness::session::should_compact(context_tokens, context_window, settings)) {
        if (co_await run_auto_compaction(false, "threshold")) {
            co_return AutoCompactionOutcome::Compacted;
        }
    }
    co_return AutoCompactionOutcome::None;
}

harness::session::CompactionSettings AgentSession::Impl::effective_compaction_settings() const {
    harness::session::CompactionSettings settings = harness::session::kDefaultCompactionSettings;
    if (!services_.settings_manager) {
        return settings;
    }
    const auto& configured = services_.settings_manager->settings().compaction;
    if (!configured) {
        return settings;
    }
    if (configured->enabled) {
        settings.enabled = *configured->enabled;
    }
    if (configured->reserve_tokens) {
        settings.reserve_tokens = static_cast<std::size_t>(*configured->reserve_tokens);
    }
    if (configured->keep_recent_tokens) {
        settings.keep_recent_tokens = static_cast<std::size_t>(*configured->keep_recent_tokens);
    }
    return settings;
}

std::optional<ai::TimestampMs> AgentSession::Impl::latest_compaction_timestamp() const {
    const auto store_path = session_.store ? session_.store->path() : std::nullopt;
    if (!store_path) {
        return std::nullopt;
    }
    // get_branch is leaf-to-root; the first compaction encountered is the
    // latest on the active path (pi `getLatestCompactionEntry`).
    for (const auto& entry : session_.store->get_branch()) {
        if (entry.kind == harness::session::SessionEntryKind::Compaction) {
            return entry.timestamp;
        }
    }
    return std::nullopt;
}

// ── Lazy-coroutine session entry ────────────────────────────────────────────

boost::asio::awaitable<support::Expected<CompactionResult>> detail::session_compact(
        std::shared_ptr<AgentSession::Impl> impl, std::string custom_instructions) {
    if (!impl) {
        co_return std::unexpected(support::make_error(support::ErrorCode::Validation, "session is not initialized"));
    }
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
    try {
#endif
        co_return co_await impl->compact(std::move(custom_instructions));
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
    } catch (const std::exception& error) {
        co_return std::unexpected(
                support::make_error(support::ErrorCode::Unknown, "session compact coroutine failed", error.what()));
    } catch (...) {
        co_return std::unexpected(support::make_error(support::ErrorCode::Unknown, "session compact coroutine failed"));
    }
#endif
}

} // namespace cch::coding_agent

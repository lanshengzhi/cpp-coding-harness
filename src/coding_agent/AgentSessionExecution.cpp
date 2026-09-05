#include "coding_agent/AgentSessionImpl.hpp"

#include <cch/coding_agent/AuthGuidance.hpp>
#include <cch/agent/harness/session/SessionStore.hpp>

#include "agent/AgentMessageAccess.hpp"
#include "agent/AgentPromptAccess.hpp"
#include "support/AsyncResultBridge.hpp"
#include "ai/utils/RetryClassifier.hpp"
#include "coding_agent/BoundedText.hpp"
#include "coding_agent/prompt/PromptExpansion.hpp"
#include "coding_agent/prompt/SystemPromptBuilder.hpp"
#include "coding_agent/runtime/AuthGuidanceStream.hpp"
#include "coding_agent/runtime/SessionEventCommitment.hpp"
#include "agent/harness/RuntimeRoot.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/system_executor.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <exception>
#include <optional>
#include <stop_token>
#include <utility>

namespace cch::coding_agent {
namespace {

/// The most recent assistant message in live history (pi
/// `_findLastAssistantMessage`), which the automatic compaction policy checks
/// after each completed loop run.
[[nodiscard]] std::optional<ai::AssistantMessage> last_assistant_message_from(
        const std::vector<ai::MessageVariant>& history) {
    for (auto it = history.rbegin(); it != history.rend(); ++it) {
        if (const auto* am = std::get_if<ai::AssistantMessage>(&*it)) {
            return *am;
        }
    }
    return std::nullopt;
}

/// pi's verbatim overflow-recovery failure message (`agent-session.ts`
/// `_checkCompaction`: a second overflow after one compact-and-retry
/// attempt).
inline constexpr std::string_view kOverflowRecoveryFailedMessage =
        "Context overflow recovery failed after one compact-and-retry attempt. "
        "Try reducing context or switching to a larger-context model.";

/// Bounded, redacted session-event observer diagnostic (ADR 0017): the
/// session-assembly mirror of the Agent's weak-observer diagnostics channel.
constexpr std::size_t kMaxSessionObserverDiagnostics = 16;
constexpr std::size_t kMaxSessionObserverDetailBytes = 1024;

void record_session_observer_diagnostic(std::vector<support::Error>& diagnostics, const support::Error& failure) {
    std::string detail = failure.message;
    if (!failure.detail.empty()) {
        detail += ": ";
        detail += failure.detail;
    }
    detail = support::bounded_redacted_text(std::move(detail), kMaxSessionObserverDetailBytes, "...");
    if (diagnostics.size() == kMaxSessionObserverDiagnostics) {
        diagnostics.erase(diagnostics.begin());
    }
    diagnostics.push_back(support::make_error(failure.code, "session event observer failed", std::move(detail)));
}

} // namespace

ai::UserMessage detail::make_admitted_user_message(std::string text,
        const std::vector<Skill>& skills,
        const std::vector<PromptTemplate>& templates,
        std::vector<ai::ImageContent> images,
        bool expand_prompt_templates) {
    auto expanded = prompt::expand_prompt_input(std::move(text), skills, templates, expand_prompt_templates);
    auto message = ai::user_text_message(std::move(expanded));
    auto& blocks = std::get<std::vector<ai::Content>>(message.content);
    blocks.reserve(blocks.size() + images.size());
    for (auto& image : images) {
        blocks.emplace_back(std::move(image));
    }
    return message;
}

ai::ModelStreamFactory AgentSession::Impl::make_stream_factory() {
    auto models = services_.model_runtime->ai_models();
    auto runtime = services_.model_runtime;
    return ai::ModelStreamFactory{[models, runtime](ai::Model model,
                                          ai::AiContext context,
                                          ai::SimpleStreamOptions options) -> ai::ModelStream {
        // The provider identity must survive the move into the Models
        // call; the forwarding sink may fire after the model argument is
        // already moved-from.
        const std::string provider{model.provider};
        auto inner = models->stream(std::move(model), std::move(context), std::move(options));
        return runtime::apply_auth_guidance(std::move(inner),
                provider,
                runtime::OAuthProviderPredicate{
                        [runtime](std::string_view provider_id) { return runtime->is_using_oauth(provider_id); }},
                std::filesystem::path{kDefaultAuthGuidanceDocsPath});
    }};
}

AgentSession::Impl::Impl(runtime::AgentSessionAssembly assembly)
    : services_(std::move(assembly.services)), session_(std::move(assembly.session)),
      skills_(std::move(assembly.skills)), templates_(std::move(assembly.templates)),
      config_(std::move(assembly.config)), session_path_(std::move(assembly.session_path)) {
    // The Session Event Commitment channel exists only for persistent
    // sessions with a Runtime mailbox; in-memory sessions commit the same
    // message kinds straight into the store's live tree (pi's non-persisting
    // SessionManager keeps the same in-memory entries; ADR 0040).
    if (session_.store && session_.store->path() && services_.runtime_target) {
        persistence_ = std::make_shared<runtime::SessionPersistence>(session_.store, services_.runtime_target);
    }
    // Seed the session's scoped-model set (pi `scopedModels: config.scopedModels`).
    scoped_models_ = config_.scoped_models;
    agent::AsyncAgentOptions options;
    options.max_queued_messages = config_.max_queued_messages;
    options.max_queued_bytes = config_.max_queued_bytes;
    options.max_turns = config_.max_turns;
    options.model = std::move(config_.model);
    // The session id is forwarded as the per-turn `sessionId` streamSimple
    // option (pi harness `sessionMetadata.id`).
    options.session_id = session_.metadata.session_id;
    // pi `sdk.ts` wires `convertToLlm` (`core/messages.ts`, which drops
    // `excludeFromContext` bash messages) into the Agent at construction; the
    // deleted `transform_context` hook's filter re-homes here, exactly like
    // pi's harness boundary (agent-loop.ts `streamAssistantResponse`). The
    // provider conversion layer repeats the drop defensively.
    options.convert_to_llm = [](std::vector<ai::MessageVariant> messages) {
        std::erase_if(messages, [](const ai::MessageVariant& message) {
            const auto* bash = std::get_if<ai::BashExecutionMessage>(&message);
            return bash != nullptr && bash->exclude_from_context;
        });
        return support::AsyncResult<std::vector<ai::MessageVariant>>{std::move(messages)};
    };
    // The System Prompt is built at session construction in pi's exact shape
    // (ADR 0036 G4; `core/agent-session.ts` `_rebuildSystemPrompt` +
    // `core/system-prompt.ts` `buildSystemPrompt`) and flows into every run
    // through `AgentContext.system_prompt`, exactly like pi's
    // `agent.state.systemPrompt`; `/reload` rebuilds it (`reload()` →
    // `rebuild_system_prompt`). The resource loader's P20 inputs land
    // here: the custom prompt (`--system-prompt` / SYSTEM.md), the append
    // strings joined with `"\n\n"` (`--append-system-prompt` /
    // APPEND_SYSTEM.md), and the Project Context Files (never trust-gated).
    // The tool prompt metadata is retained before the registry moves into
    // the Agent below so `/reload` can rebuild the same shape.
    {
        static constexpr std::array kFixedToolNames{"read", "bash", "edit", "write"};
        for (const char* name : kFixedToolNames) {
            auto metadata = services_.tools.prompt_metadata(name);
            if (!metadata) {
                continue;
            }
            prompt_selected_tools_.emplace_back(name);
            if (metadata->snippet) {
                prompt_tool_snippets_.emplace(name, *metadata->snippet);
            }
            prompt_tool_guidelines_.insert(
                    prompt_tool_guidelines_.end(), metadata->guidelines.begin(), metadata->guidelines.end());
        }
    }
    options.system_prompt = rebuild_system_prompt();

    // Resumed history is transferred exactly once into the authoritative Agent
    // state. AgentSession retains product metadata and durable storage only.
    agent::AgentInitialState initial_state;
    initial_state.messages = std::move(session_.history);
    // Thinking level through pi's session-creation chain (sdk.ts): a resumed
    // `thinking_level_change` entry wins, then the settings
    // `defaultThinkingLevel`, then pi's DEFAULT_THINKING_LEVEL ("medium"); the
    // Agent clamps the request against the resolved model at construction
    // (ADR 0034 / #352 / T04).
    initial_state.thinking_level =
            session_.context_thinking_level.value_or(config_.default_thinking_level.value_or("medium"));

    // Request-time re-auth guidance (pi `_getRequiredRequestAuth`): the
    // Agent's stream and the summarization seam run through a session-layer
    // ModelStream decorator that rewrites auth/oauth-category terminal
    // failures to pi's two verbatim guidance branches.

    // Construct Agent last: it holds the AI-owned ModelStream factory (ADR
    // 0040 / #453) and takes sole ownership of the move-only tool registry.
    agent_.emplace(make_stream_factory(), std::move(services_.tools), std::move(options), std::move(initial_state));

    // Expose the live session facts to the model Bash Tool (pi
    // `resolveSpawnContext`); the Agent's clamped state is authoritative.
    refresh_bash_session_environment();
    current_snapshot_.store(std::make_shared<const AgentSessionSnapshot>(create_snapshot()), std::memory_order_release);
    state_version_.store(1, std::memory_order_release);
}

std::string AgentSession::Impl::rebuild_system_prompt() const {
    // pi `_rebuildSystemPrompt` (`core/agent-session.ts`) + `buildSystemPrompt`
    // (`core/system-prompt.ts`): the default/custom branches, tool snippets +
    // guidelines, `<project_context>`, the skills section, and the cwd line,
    // with the identity delta confined to the documentation paths. The tool
    // metadata is the retained collection (the move-only tool registry moved
    // into the Agent at construction).
    prompt::BuildSystemPromptOptions prompt_options;
    prompt_options.customPrompt = config_.custom_prompt;
    // pi `_rebuildSystemPrompt`: append strings join with `"\n\n"`; an
    // empty list appends nothing.
    if (!config_.append_system_prompt.empty()) {
        std::string joined = config_.append_system_prompt.front();
        for (std::size_t index = 1; index < config_.append_system_prompt.size(); ++index) {
            joined += "\n\n";
            joined += config_.append_system_prompt[index];
        }
        prompt_options.appendSystemPrompt = std::move(joined);
    }
    prompt_options.contextFiles = config_.context_files;
    // pi `_refreshToolRegistry` → `_toolPromptSnippets`/`_toolPromptGuidelines`:
    // prompt metadata for the active tools, retained from construction (pi's
    // default active tool order `["read", "bash", "edit", "write"]`).
    prompt_options.selectedTools = prompt_selected_tools_;
    prompt_options.toolSnippets = prompt_tool_snippets_;
    prompt_options.promptGuidelines.insert(
            prompt_options.promptGuidelines.end(), prompt_tool_guidelines_.begin(), prompt_tool_guidelines_.end());
    prompt_options.cwd = session_.workspace.string();
    prompt_options.skills = skills_;
    // Identity delta: the C++ binary's own documentation paths (pi
    // `config.ts` `getReadmePath`/`getDocsPath`/`getExamplesPath` resolve the
    // pi package; pike resolves its own source tree).
#ifndef CCH_SOURCE_DIR
    constexpr std::string_view kSourceDir = "";
#else
    constexpr std::string_view kSourceDir = CCH_SOURCE_DIR;
#endif
    prompt_options.readmePath = std::string{kSourceDir} + "/README.md";
    prompt_options.docsPath = std::string{kSourceDir} + "/docs";
    prompt_options.examplesPath = std::string{kSourceDir} + "/examples";
    return buildSystemPrompt(prompt_options);
}

support::ExpectedVoid AgentSession::Impl::reject_if_closed() const {
    if (lifecycle_ != Lifecycle::Open) {
        return std::unexpected(support::make_error(support::ErrorCode::Validation, "session is closed"));
    }
    return {};
}

support::ExpectedVoid AgentSession::Impl::reject_if_busy() const {
    if (prompt_active_) {
        return std::unexpected(
                support::make_error(support::ErrorCode::Validation, "session is busy (prompt already in flight)"));
    }
    if (reload_active_) {
        return std::unexpected(support::make_error(support::ErrorCode::Busy, "session reload is already in flight"));
    }
    if (compaction_active_) {
        return std::unexpected(
                support::make_error(support::ErrorCode::Validation, "session is busy (compaction already in flight)"));
    }
    return {};
}

support::ExpectedVoid AgentSession::Impl::reject_if_user_bash_busy() const {
    if (user_bash_active_) {
        return std::unexpected(
                support::make_error(support::ErrorCode::Validation, "a User Bash command is already in flight"));
    }
    return {};
}

boost::asio::awaitable<support::ExpectedVoid> AgentSession::Impl::preflight_auth_guidance() {
    if (!agent_ || !services_.model_runtime) {
        co_return support::ExpectedVoid{};
    }
    const auto& model = agent_->state().model;
    // The placeholder kDefaultModel is the C++ "no model" state; "no model"
    // is not an auth failure, and streaming it fails through normal provider
    // lookup ("Unknown provider: unknown") exactly like pi.
    if (model.id == agent::detail::kDefaultModel.id) {
        co_return support::ExpectedVoid{};
    }
    // pi `prompt()`: `hasConfiguredAuth(provider) ||
    // (await checkAuth(provider)) !== undefined`. The live `checkAuth` is the
    // authoritative backstop (the snapshot may be stale); it is
    // side-effect-free and never refreshes OAuth.
    if (services_.model_runtime->has_configured_auth(model.provider)) {
        co_return support::ExpectedVoid{};
    }
    auto checked = co_await support::detail::await_async_result(services_.model_runtime->check_auth(model.provider));
    if (!checked) {
        co_return std::unexpected(std::move(checked.error()));
    }
    if (*checked) {
        co_return support::ExpectedVoid{};
    }
    const std::string provider{model.provider};
    if (services_.model_runtime->is_using_oauth(provider)) {
        co_return std::unexpected(
                support::make_error(support::ErrorCode::Auth, format_oauth_reauthenticate_message(provider)));
    }
    co_return std::unexpected(support::make_error(support::ErrorCode::Auth,
            format_no_api_key_found_message(provider, std::filesystem::path{kDefaultAuthGuidanceDocsPath})));
}

boost::asio::awaitable<support::ExpectedVoid> AgentSession::Impl::run_prompt(
        std::string prompt, std::vector<ai::ImageContent> images, bool expand_prompt_templates) {
    if (auto rejected = reject_if_closed(); !rejected) {
        co_return std::unexpected(rejected.error());
    }
    if (auto rejected = reject_if_busy(); !rejected) {
        co_return std::unexpected(rejected.error());
    }
    // ADR 0040: a recorded persistence failure is session-scoped sticky
    // state; live state is never rolled back and later prompts are rejected
    // with the typed failure.
    if (persistence_) {
        if (auto failure = persistence_->failure()) {
            co_return std::unexpected(support::make_error(support::ErrorCode::Session,
                    "session persistence failed; rejecting new prompt",
                    failure->detail.empty() ? failure->message : failure->detail));
        }
    } else if (session_.store && session_.store->path()) {
        co_return std::unexpected(support::make_error(
                support::ErrorCode::Session, "session persistence is unavailable; rejecting new prompt"));
    }

    prompt_active_ = true;
    active_stop_source_.emplace();
    // A concurrent manual compaction awaits this signal after requesting run
    // cancellation; it is cancelled exactly when the run settles (the same
    // waiter-before-cancel ordering as PendingUserBashCommit).
    prompt_settled_signal_.emplace(co_await boost::asio::this_coro::executor);
    prompt_settled_signal_->expires_at(std::chrono::steady_clock::time_point::max());

    support::ExpectedVoid result;
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
    try {
#endif
        ai::UserMessage user_message = detail::make_admitted_user_message(
                std::move(prompt), skills_, templates_, std::move(images), expand_prompt_templates);

        // pi `prompt()` auth preflight: a real model whose provider has no
        // configured auth fails with pi's verbatim re-auth guidance
        // before the run starts (the `kDefaultModel` placeholder is
        // skipped and keeps its ordinary "Unknown provider: unknown"
        // streaming failure).
        if (auto admitted = co_await preflight_auth_guidance(); !admitted) {
            result = std::unexpected(admitted.error());
        } else {
            // pi AgentSession.prompt pre-send compaction check (catches
            // aborted responses and unhandled error terminals from the
            // previous run): the last assistant message may still push context
            // over the threshold. The user's new prompt below is the
            // continuation, so no retry is performed here (pi: "do not call
            // agent.continue() here").
            const auto last_assistant = last_assistant_message_from(agent_->state().messages);
            if (last_assistant) {
                const auto preflight_outcome =
                        co_await check_auto_compaction(*last_assistant, /*skip_aborted_check=*/false);
                (void)preflight_outcome;
            }
            // pi resets the overflow-recovery attempt when a new user message
            // starts; the pre-prompt check above still observes the previous
            // attempt's state.
            overflow_recovery_attempted_ = false;
            result = co_await run_agent_loop(std::move(user_message), *active_stop_source_);
        }
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
    } catch (const std::exception& error) {
        result = std::unexpected(
                support::make_error(support::ErrorCode::Unknown, "session prompt coroutine failed", error.what()));
    } catch (...) {
        result = std::unexpected(support::make_error(support::ErrorCode::Unknown, "session prompt coroutine failed"));
    }
#endif

    active_stop_source_.reset();
    // The whole run (including steering and follow-up continuations) has
    // settled: commit every Bash that completed mid-run exactly once, in
    // completion order, before close finalization releases the store. This is
    // also the flush point that guarantees a later idle Prompt builds provider
    // context only after every completed Bash committed.
    flush_pending_user_bash();
    prompt_active_ = false;
    if (prompt_settled_signal_) {
        (void)prompt_settled_signal_->cancel();
        // Release the host-executor timer with the run. cancel() completes
        // every queued waiter (wait_for_idle/compact); their posted
        // completions do not touch the timer afterwards. Resetting here keeps
        // the Asio object inside the live host loop's lifetime: the host
        // io_context (e.g. prompt_blocking's per-call loop) may be destroyed
        // before the session, and a timer destructed after its context reads
        // a freed service registry (ASan, issue #473).
        prompt_settled_signal_.reset();
    }
    if (lifecycle_ == Lifecycle::Closing) {
        // The prompt awaitable is the existing observation seam for active
        // close: owned environment cleanup finishes before it settles. An
        // overlapping User Bash or manual compaction finalizes close when it
        // is the last active work instead (issue #467).
        if (!user_bash_active_ && !compaction_active_ && !reload_active_) {
            co_await finalize_close_after_active_work();
        }
    }
    co_return result;
}

boost::asio::awaitable<support::ExpectedVoid> AgentSession::Impl::run_agent_loop(
        ai::UserMessage prompt, std::stop_source stop_source) {
    if (!agent_) {
        co_return std::unexpected(support::make_error(support::ErrorCode::Validation, "session Agent is unavailable"));
    }

    runtime::SessionEventCommitment commitment{persistence_, session_.store};
    // The commitment sink also observes assistant message endings so the turn
    // auto-retry success event fires at the first non-error assistant message
    // (pi `_handleAgentEvent` message_end handler resets `_retryAttempt` and
    // emits `auto_retry_end success`). Rebuilt per call because the wrapped
    // sink is move-only and each prompt/continue takes it by value.
    const auto make_retry_observing_sink = [&]() {
        return agent::AgentEventCommitter{
                [this, inner = commitment.sink()](
                        const agent::AgentLifecycleEvent& event) mutable -> support::ExpectedVoid {
                    if (retry_attempt_ > 0) {
                        if (const auto* end = std::get_if<agent::MessageEndEvent>(&event)) {
                            const auto* assistant = std::get_if<ai::AssistantMessage>(&end->message);
                            if (assistant != nullptr && assistant->stop_reason != ai::AssistantStopReason::Error) {
                                emit_session_event(AutoRetryEndEvent{
                                        .success = true,
                                        .attempt = retry_attempt_,
                                });
                                retry_attempt_ = 0;
                            }
                        }
                    }
                    return inner(event);
                }};
    };

    std::optional<support::ExpectedVoid> result;
    result = co_await support::detail::await_async_result(agent::detail::AgentPromptAccess::prompt(
            *agent_, std::move(prompt), make_retry_observing_sink(), stop_source));
    if (!result) {
        co_return co_await commitment.conclude(std::move(result));
    }

    // Post-run loop in pi `_handlePostAgentRun` order: turn auto-retry (T12)
    // first, then the automatic compaction trigger (T10). Overflow errors are
    // never retryable (`is_retryable_error` excludes them), so the two
    // recovery paths never interfere: overflow routes to compact-and-retry
    // exactly once, while transient provider/network errors retry with
    // exponential backoff through the agent continuation mechanism.
    for (;;) {
        const auto last_assistant = last_assistant_message_from(agent_->state().messages);
        if (!last_assistant) {
            break;
        }

        if (is_retryable_error(*last_assistant)) {
            if (co_await prepare_retry(*last_assistant, stop_source.get_token())) {
                result = co_await support::detail::await_async_result(agent::detail::AgentPromptAccess::continue_run(
                        *agent_, make_retry_observing_sink(), stop_source));
                if (!result) {
                    break;
                }
                continue;
            }
        }
        if (last_assistant->stop_reason == ai::AssistantStopReason::Error && retry_attempt_ > 0) {
            // The final retry attempt failed: emit `auto_retry_end` so the
            // retry cycle is observable end to end (pi
            // `_handlePostAgentRun` failure branch).
            emit_session_event(AutoRetryEndEvent{
                    .success = false,
                    .attempt = retry_attempt_,
                    .final_error = last_assistant->error_message,
            });
            retry_attempt_ = 0;
        }

        const auto outcome = co_await check_auto_compaction(*last_assistant, /*skip_aborted_check=*/true);
        if (outcome == AutoCompactionOutcome::OverflowRecoveryFailed) {
            // The run's messages (including the second overflow error) are
            // persisted through the commitment; the prompt fails with pi's
            // verbatim recovery message (the failure the `compaction_end`
            // event carries in pi).
            co_return co_await commitment.conclude(std::optional<support::ExpectedVoid>{std::unexpected(
                    support::make_error(support::ErrorCode::Stream, std::string{kOverflowRecoveryFailedMessage}))});
        }
        if (outcome != AutoCompactionOutcome::OverflowRetry) {
            break;
        }
        result = co_await support::detail::await_async_result(
                agent::detail::AgentPromptAccess::continue_run(*agent_, make_retry_observing_sink(), stop_source));
        if (!result) {
            break;
        }
    }
    co_return co_await commitment.conclude(std::move(result));
}

void AgentSession::Impl::emit_session_event(const AgentSessionEvent& event) {
    if (session_event_observers_.empty()) {
        return;
    }
    // Stable per-event snapshot: reentrant subscribe/unsubscribe mutate the
    // live registry without invalidating this delivery pass. Each shared
    // entry's active flags are re-checked immediately before invocation, so
    // an unsubscribe earlier in the pass suppresses a later subscriber's turn
    // and a new subscription begins with the next event.
    const auto snapshot = session_event_observers_;
    for (const auto& subscriber : snapshot) {
        if (!subscriber->delivery_enabled || !subscriber->sink) {
            continue;
        }
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
        try {
#endif
            if (auto observed = subscriber->sink(event); !observed) {
                // A failing observer is deactivated and never vetoes retry
                // progress or persistence (ADR 0017); its failure is recorded
                // in the session's bounded, redacted diagnostics channel.
                record_session_observer_diagnostic(session_event_diagnostics_, observed.error());
                subscriber->registered = false;
                subscriber->delivery_enabled = false;
            }
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
        } catch (const std::exception& exception) {
            record_session_observer_diagnostic(
                    session_event_diagnostics_, support::make_error(support::ErrorCode::Unknown, exception.what()));
            subscriber->registered = false;
            subscriber->delivery_enabled = false;
        } catch (...) {
            record_session_observer_diagnostic(
                    session_event_diagnostics_, support::make_error(support::ErrorCode::Unknown, "unknown exception"));
            subscriber->registered = false;
            subscriber->delivery_enabled = false;
        }
#endif
    }
    std::erase_if(session_event_observers_,
            [](const std::shared_ptr<SessionSubscriber>& subscriber) { return !subscriber->registered; });
}

// ── Turn auto-retry (T12) ───────────────────────────────────────────────────

RetrySettings AgentSession::Impl::effective_retry_settings() const {
    RetrySettings settings;
    if (!services_.settings_manager) {
        return settings;
    }
    const auto& configured = services_.settings_manager->settings().retry;
    if (!configured) {
        return settings;
    }
    if (configured->enabled) {
        settings.enabled = *configured->enabled;
    }
    if (configured->max_retries) {
        settings.max_retries = static_cast<std::size_t>(*configured->max_retries);
    }
    if (configured->base_delay_ms) {
        settings.base_delay_ms = static_cast<std::size_t>(*configured->base_delay_ms);
    }
    return settings;
}

bool AgentSession::Impl::is_retryable_error(const ai::AssistantMessage& message) const {
    // Context overflow is handled by compaction, never by retry (pi
    // `_isRetryableError`); the two recovery paths never interfere (T10's
    // boundary).
    const auto& model = agent_->state().model;
    const std::size_t context_window = static_cast<std::size_t>(model.context_window);
    if (harness::session::is_context_overflow(message, context_window)) {
        return false;
    }
    return ai::is_retryable_assistant_error(message);
}

boost::asio::awaitable<bool> AgentSession::Impl::prepare_retry(
        const ai::AssistantMessage& message, std::stop_token stop_token) {
    const auto settings = effective_retry_settings();
    if (!settings.enabled) {
        co_return false;
    }

    ++retry_attempt_;
    if (retry_attempt_ > static_cast<int>(settings.max_retries)) {
        // Preserve the completed attempt count so post-run handling can emit
        // the final failure (pi `_prepareRetry` decrements back).
        --retry_attempt_;
        co_return false;
    }

    const auto delay_ms = settings.base_delay_ms * (static_cast<std::size_t>(1) << (retry_attempt_ - 1));
    emit_session_event(AutoRetryStartEvent{
            .attempt = retry_attempt_,
            .max_attempts = static_cast<int>(settings.max_retries),
            .delay_ms = static_cast<std::int64_t>(delay_ms),
            .error_message = message.error_message.value_or("Unknown error"),
    });

    // Remove the failed assistant message from live state; it stays in
    // session history (pi `_prepareRetry` `messages.slice(0, -1)`), so the
    // continuation's last message is a user or tool-result message.
    if (auto popped = agent::detail::AgentMessageAccess::pop_trailing_assistant(*agent_); !popped) {
        co_return false;
    }

    // Abort-interruptible exponential backoff sleep (pi `sleep(delayMs,
    // this._retryAbortController.signal)`): a prompt-scoped abort cancels the
    // timer, and an already-requested stop aborts the wait immediately.
    boost::asio::steady_timer timer(co_await boost::asio::this_coro::executor);
    timer.expires_after(std::chrono::milliseconds(delay_ms));
    boost::system::error_code wait_error;
    std::stop_callback cancel_wait{stop_token, [&timer]() { timer.cancel(); }};
    co_await timer.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, wait_error));
    if (wait_error == boost::asio::error::operation_aborted || stop_token.stop_requested()) {
        // Aborted during backoff: emit the end event so observers can clean
        // up, and produce exactly one terminal outcome (the retry never
        // starts) — pi's `_prepareRetry` catch branch.
        const int attempt = retry_attempt_;
        retry_attempt_ = 0;
        emit_session_event(AutoRetryEndEvent{
                .success = false,
                .attempt = attempt,
                .final_error = std::string{"Retry cancelled"},
        });
        co_return false;
    }
    co_return true;
}

boost::asio::awaitable<support::ExpectedVoid> AgentSession::Impl::wait_for_idle() {
    // pi `waitForIdle`: settle when an Agent run is active. The run in
    // flight continues to its normal terminal; the settled signal is
    // cancelled exactly when the run settles (same waiter-before-cancel
    // ordering as PendingUserBashCommit). The wait itself cannot fail.
    if (!prompt_active_ || !prompt_settled_signal_) {
        co_return support::ExpectedVoid{};
    }
    boost::system::error_code wait_error;
    co_await prompt_settled_signal_->async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, wait_error));
    co_return support::ExpectedVoid{};
}

void AgentSession::Impl::abort() {
    if (prompt_active_ && active_stop_source_) {
        (void)active_stop_source_->request_stop();
    }
}

void AgentSession::Impl::close() noexcept {
    if (lifecycle_ != Lifecycle::Open) {
        return;
    }
    lifecycle_ = Lifecycle::Closing;
    update_projection();
    // Admission stopped above before any cancellation request below (issue
    // #467): every entry point's reject_if_closed observes Closing first.
    // Request work-scoped cancellation but retain the active loop, callbacks,
    // commitment, store, and capabilities until each admitted operation
    // (prompt, User Bash, manual compaction) unwinds through its ordinary
    // lifecycle. An admitted compaction is awaited, not cancelled (ADR 0040:
    // Close waits for admitted compaction work to reach its terminal
    // outcome). The last active work to settle finalizes the close.
    if (prompt_active_ && active_stop_source_) {
        (void)active_stop_source_->request_stop();
    }
    if (user_bash_active_ && active_user_bash_stop_source_) {
        (void)active_user_bash_stop_source_->request_stop();
    }
    if (reload_active_) {
        (void)reload_stop_source_.request_stop();
    }
    if (!prompt_active_ && !user_bash_active_ && !compaction_active_ && !reload_active_) {
        finalize_close();
    }
}

std::shared_ptr<harness::AsyncFileSystem> AgentSession::Impl::release_close_resources() noexcept {
    if (agent_) {
        agent_->clear_subscriptions();
    }
    agent_.reset();
    skills_.clear();
    templates_.clear();
    session_.store.reset();
    services_.user_shell.reset();
    if (services_.model_runtime_owned) {
        services_.model_runtime.reset();
    }

    // The session always owns its filesystem capability; Session Close runs
    // its temporary-resource cleanup after Tool work has quiesced (ADR 0048).
    return std::move(services_.filesystem);
}

boost::asio::awaitable<void> AgentSession::Impl::finalize_close_after_active_work() {
    if (lifecycle_ == Lifecycle::Closed) {
        co_return;
    }
    // Every admitted Session Event Commitment reaches its terminal
    // persistence outcome before the store is released (ADR 0040, issue
    // #467). The settling work already drained the channel (the prompt
    // commitment's conclude(), the compaction's mid-run drain), so this wait
    // is the explicit Close-time quiescence point rather than new blocking.
    if (persistence_) {
        co_await persistence_->drain();
    }
    auto owned_filesystem = release_close_resources();
    if (owned_filesystem) {
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
        try {
#endif
            (void)co_await support::detail::await_async_result(owned_filesystem->cleanup());
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
        } catch (...) {
            // cleanup() is best-effort and must not make close fallible.
        }
#endif
    }
    lifecycle_ = Lifecycle::Closed;
}

void AgentSession::Impl::finalize_close() noexcept {
    if (lifecycle_ == Lifecycle::Closed) {
        return;
    }
    // Idle close: no prompt, User Bash, or compaction is admitted, so no
    // Session Event Commitment can still be in flight (submissions only
    // happen inside an admitted run whose settle drains the channel).
    auto owned_filesystem = release_close_resources();
    lifecycle_ = Lifecycle::Closed;

    // Idle close has no host executor to await. Transfer the owned filesystem
    // to a posted best-effort cleanup task on the session's Runtime loop when
    // one exists, so the final application Close drain quiesces process
    // resources before the loop stops (ADR 0040, issue #467); a session
    // assembled without a Runtime root falls back to the shared system
    // executor.
    if (owned_filesystem) {
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
        try {
#endif
            // post() prevents a cleanup coroutine from executing inline on the
            // close() stack before its first suspension point.
            const auto cleanup_executor = services_.runtime_target
                                                  ? services_.runtime_target->executor()
                                                  : boost::asio::any_io_executor{boost::asio::system_executor{}};
            boost::asio::post(cleanup_executor, [filesystem = std::move(owned_filesystem), cleanup_executor]() mutable {
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
                try {
#endif
                    boost::asio::co_spawn(
                            cleanup_executor,
                            [filesystem = std::move(filesystem)]() -> boost::asio::awaitable<void> {
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
                                try {
#endif
                                    (void)co_await support::detail::await_async_result(filesystem->cleanup());
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
                                } catch (...) {
                                    // cleanup() is best-effort and must not make close fallible.
                                }
#endif
                            },
                            boost::asio::detached);
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
                } catch (...) {
                    // Launch remains best-effort after close has released ownership.
                }
#endif
            });
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
        } catch (...) {
            // Scheduling is also best-effort; close remains noexcept.
        }
#endif
    }
}

// ── Lazy-coroutine session entries ──────────────────────────────────────────

boost::asio::awaitable<support::ExpectedVoid> detail::session_prompt(std::shared_ptr<AgentSession::Impl> impl,
        std::string text,
        std::vector<ai::ImageContent> images,
        bool expand_prompt_templates) {
    if (!impl) {
        co_return std::unexpected(support::make_error(support::ErrorCode::Validation, "session is not initialized"));
    }
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
    try {
#endif
        co_return co_await impl->run_prompt(std::move(text), std::move(images), expand_prompt_templates);
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
    } catch (const std::exception& error) {
        co_return std::unexpected(
                support::make_error(support::ErrorCode::Unknown, "session prompt coroutine failed", error.what()));
    } catch (...) {
        co_return std::unexpected(support::make_error(support::ErrorCode::Unknown, "session prompt coroutine failed"));
    }
#endif
}

boost::asio::awaitable<support::ExpectedVoid> detail::session_wait_for_idle(std::shared_ptr<AgentSession::Impl> impl) {
    if (impl) {
        (void)co_await impl->wait_for_idle();
    }
    co_return support::ExpectedVoid{};
}

} // namespace cch::coding_agent

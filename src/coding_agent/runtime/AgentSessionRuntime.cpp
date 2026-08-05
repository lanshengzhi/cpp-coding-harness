#include "AgentSessionRuntime.hpp"

#include <cch/ai/Content.hpp>
#include <cch/coding_agent/Settings.hpp>
#include <cch/harness/session/JsonlSessionStore.hpp>

#include "agent/AgentMessageAccess.hpp"
#include "agent/AgentPromptAccess.hpp"
#include "coding_agent/BoundedText.hpp"
#include "coding_agent/SkillFormatting.hpp"
#include "coding_agent/runtime/UserBashOutputAccumulator.hpp"
#include "harness/compaction/Compaction.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/system_executor.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <algorithm>
#include <chrono>
#include <exception>
#include <optional>
#include <stop_token>
#include <utility>

namespace cch::coding_agent::runtime {

namespace {

[[nodiscard]] std::optional<std::string> last_assistant_text_from(
    const std::vector<ai::MessageVariant>& history) {
    for (auto it = history.rbegin(); it != history.rend(); ++it) {
        if (const auto* am = std::get_if<ai::AssistantMessage>(&*it)) {
            return ai::text_from_assistant_content(am->content);
        }
    }
    return std::nullopt;
}

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

[[nodiscard]] util::ExpectedVoid prompt_exception(std::exception_ptr exception) {
    try {
        std::rethrow_exception(exception);
    } catch (const std::exception& error) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Unknown,
            "session prompt coroutine failed",
            error.what()));
    } catch (...) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Unknown,
            "session prompt coroutine failed",
            "unknown exception"));
    }
}

[[nodiscard]] ai::TimestampMs completion_timestamp_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

[[nodiscard]] ai::BashExecutionMessage make_bash_execution_message(
    const UserShellResult& result,
    const UserBashOutputAccumulator& output,
    std::string command,
    bool exclude_from_context,
    ai::TimestampMs timestamp) {
    ai::BashExecutionMessage message;
    message.command = std::move(command);
    // The bounded sanitized tail is the model-context value; the spill path
    // is recorded alongside it, never substituted for it.
    message.output = output.tail();
    message.exit_code = result.cancelled ? std::nullopt : result.exit_code;
    message.cancelled = result.cancelled;
    message.truncated = output.truncated();
    if (output.full_output_path()) {
        message.full_output_path = *output.full_output_path();
    }
    message.exclude_from_context = exclude_from_context;
    message.timestamp = timestamp;
    return message;
}

} // namespace

AgentSessionRuntime::AgentSessionRuntime(
    RuntimeServices services,
    OpenSession session,
    prompt::PromptProcessor prompt_processor,
    AgentSessionRuntimeConfig config)
    : services_(std::move(services)),
      session_(std::move(session)),
      prompt_processor_(std::move(prompt_processor)),
      config_(std::move(config)) {
    // Build the <available_skills> block once from the same immutable snapshot
    // used for explicit /skill:name invocation.
    agent::AsyncAgentOptions options;
    options.max_queued_messages = config_.max_queued_messages;
    options.max_queued_bytes = config_.max_queued_bytes;
    options.max_turns = config_.max_turns;
    options.model = std::move(config_.model);
    // The session id is forwarded as the per-turn `sessionId` streamSimple
    // option (pi harness `sessionMetadata.id`).
    options.session_id = session_.metadata.session_id;
    options.transform_context = [](
                                    std::vector<ai::MessageVariant> messages,
                                    std::stop_token) -> boost::asio::awaitable<
                                        util::Expected<std::vector<ai::MessageVariant>>> {
        std::erase_if(messages, [](const ai::MessageVariant& message) {
            const auto* bash = std::get_if<ai::BashExecutionMessage>(&message);
            return bash != nullptr && bash->exclude_from_context;
        });
        co_return messages;
    };

    std::string skills_block = formatSkillsForPrompt(prompt_processor_->skills());
    if (!skills_block.empty()) {
        auto existing_transform = std::move(options.transform_context);
        options.transform_context = [block = std::move(skills_block),
                                     existing = std::move(existing_transform)](
                                        std::vector<ai::MessageVariant> messages,
                                        std::stop_token stop_token) mutable
            -> boost::asio::awaitable<util::Expected<std::vector<ai::MessageVariant>>> {
            std::vector<ai::MessageVariant> transformed;
            if (existing) {
                auto prior = co_await (*existing)(std::move(messages), stop_token);
                if (!prior) {
                    co_return std::unexpected(prior.error());
                }
                transformed = std::move(*prior);
            } else {
                transformed = std::move(messages);
            }

            ai::SystemMessage msg;
            msg.content = block;
            transformed.insert(transformed.begin(), ai::MessageVariant{std::move(msg)});
            co_return transformed;
        };
    }

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
        session_.context_thinking_level.value_or(
            config_.default_thinking_level.value_or("medium"));

    // Construct Agent last: it holds the factory-owned ModelRuntime (the sole
    // injectable seam per #326) and takes sole ownership of the move-only tool
    // registry.
    agent_.emplace(
        services_.model_runtime,
        std::move(services_.tools),
        std::move(options),
        std::move(initial_state));
}

util::ExpectedVoid AgentSessionRuntime::reject_if_closed() const {
    if (lifecycle_ != Lifecycle::Open) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "session is closed"));
    }
    return {};
}

util::ExpectedVoid AgentSessionRuntime::reject_if_busy() const {
    if (prompt_active_) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "session is busy (prompt already in flight)"));
    }
    if (compaction_active_) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "session is busy (compaction already in flight)"));
    }
    return {};
}

util::ExpectedVoid AgentSessionRuntime::reject_if_user_bash_busy() const {
    if (user_bash_active_) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "a User Bash command is already in flight"));
    }
    return {};
}

util::ExpectedVoid AgentSessionRuntime::commit_user_bash_completion(
    UserBashCompletion& completion) {
    // Live Session State advances first; a Session Store failure is reported
    // on the completion diagnostic without rolling the message back.
    if (auto committed = agent::detail::AgentMessageAccess::append_bash_execution(
            *agent_, completion.message);
        !committed) {
        return std::unexpected(std::move(committed.error()));
    }
    if (auto persisted = session_.store->append(
            ai::MessageVariant{completion.message});
        !persisted) {
        completion.diagnostic = std::move(persisted.error());
    }
    return {};
}

void AgentSessionRuntime::flush_pending_user_bash() {
    if (pending_user_bash_.empty()) return;
    auto pending = std::exchange(pending_user_bash_, {});
    for (auto& entry : pending) {
        if (agent_ && session_.store) {
            entry->commit_result = commit_user_bash_completion(entry->completion);
        } else {
            entry->commit_result = std::unexpected(util::make_error(
                util::ErrorCode::Validation,
                "session Agent is unavailable"));
        }
        try {
            (void)entry->committed_signal.cancel();
        } catch (...) {
            // Releasing the awaiting coroutine is best-effort; the commitment
            // above is the authoritative outcome.
        }
    }
}

namespace {

/// One admission shaping for every user input path (Prompt, steering,
/// follow-up): optional Prompt Template expansion, then image content
/// appended to one complete user Agent Message.
[[nodiscard]] util::Expected<ai::UserMessage> make_admitted_user_message(
    std::optional<prompt::PromptProcessor>& prompt_processor,
    std::string text,
    std::vector<ai::ImageContent> images,
    bool expand_prompt_templates) {
    if (!prompt_processor) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "session is closed"));
    }
    auto expanded = prompt_processor->process(
        std::move(text), expand_prompt_templates);
    auto message = ai::user_text_message(std::move(expanded.text));
    auto& blocks = std::get<std::vector<ai::Content>>(message.content);
    blocks.reserve(blocks.size() + images.size());
    for (auto& image : images) {
        blocks.emplace_back(std::move(image));
    }
    return message;
}

} // namespace

boost::asio::awaitable<util::ExpectedVoid> AgentSessionRuntime::run_prompt(
    std::string prompt,
    std::vector<ai::ImageContent> images,
    bool expand_prompt_templates,
    std::move_only_function<util::ExpectedVoid()> on_preflight_accepted) {
    if (auto rejected = reject_if_closed(); !rejected) {
        co_return std::unexpected(rejected.error());
    }
    if (auto rejected = reject_if_busy(); !rejected) {
        co_return std::unexpected(rejected.error());
    }

    prompt_active_ = true;
    active_stop_source_.emplace();
    // A concurrent manual compaction awaits this signal after requesting run
    // cancellation; it is cancelled exactly when the run settles (the same
    // waiter-before-cancel ordering as PendingUserBashCommit).
    prompt_settled_signal_.emplace(co_await boost::asio::this_coro::executor);
    prompt_settled_signal_->expires_at(
        std::chrono::steady_clock::time_point::max());

    util::ExpectedVoid result;
    try {
        bool run_agent = true;
        ai::UserMessage user_message;
        if (auto message = make_admitted_user_message(
                prompt_processor_,
                std::move(prompt),
                std::move(images),
                expand_prompt_templates);
            !message) {
            result = std::unexpected(message.error());
            run_agent = false;
        } else {
            user_message = std::move(*message);
        }

        if (run_agent && on_preflight_accepted) {
            if (auto acknowledged = on_preflight_accepted(); !acknowledged) {
                result = std::unexpected(acknowledged.error());
                run_agent = false;
            }
        }
        if (run_agent) {
            // pi AgentSession.prompt pre-send compaction check (catches
            // aborted responses and unhandled error terminals from the
            // previous run): the last assistant message may still push context
            // over the threshold. The user's new prompt below is the
            // continuation, so no retry is performed here (pi: "do not call
            // agent.continue() here").
            const auto last_assistant =
                last_assistant_message_from(agent_->state().messages);
            if (last_assistant) {
                const auto preflight_outcome = co_await check_auto_compaction(
                    *last_assistant, /*skip_aborted_check=*/false);
                (void)preflight_outcome;
            }
            // pi resets the overflow-recovery attempt when a new user message
            // starts; the pre-prompt check above still observes the previous
            // attempt's state.
            overflow_recovery_attempted_ = false;
            result = co_await run_agent_loop(
                std::move(user_message),
                *active_stop_source_);
        }
    } catch (...) {
        result = prompt_exception(std::current_exception());
    }

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
    }
    if (lifecycle_ == Lifecycle::Closing) {
        // The prompt awaitable is the existing observation seam for active
        // close: owned environment cleanup finishes before it settles. An
        // overlapping User Bash finalizes close when it is the last active
        // work instead.
        if (!user_bash_active_) {
            co_await finalize_close_after_active_work();
        }
    }
    co_return result;
}

boost::asio::awaitable<util::Expected<UserBashCompletion>>
AgentSessionRuntime::run_user_bash(
    std::string command,
    bool exclude_from_context,
    UserBashProgressSink progress_sink) {
    if (auto rejected = reject_if_closed(); !rejected) {
        co_return std::unexpected(rejected.error());
    }
    if (auto rejected = reject_if_user_bash_busy(); !rejected) {
        co_return std::unexpected(rejected.error());
    }
    if (!services_.user_shell) {
        co_return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "User Shell is unavailable"));
    }

    user_bash_active_ = true;
    active_user_bash_stop_source_.emplace();
    const auto recorded_command = command;
    util::Expected<UserShellResult> shell_result = std::unexpected(util::make_error(
        util::ErrorCode::Unknown,
        "User Shell execution did not finish"));
    if (progress_sink) {
        try {
            if (auto started = progress_sink(UserBashProgress{
                    .command = recorded_command,
                    .output = {},
                    .exclude_from_context = exclude_from_context,
                });
                !started) {
                active_user_bash_stop_source_.reset();
                user_bash_active_ = false;
                co_return std::unexpected(std::move(started.error()));
            }
        } catch (const std::exception& error) {
            active_user_bash_stop_source_.reset();
            user_bash_active_ = false;
            co_return std::unexpected(util::make_error(
                util::ErrorCode::Unknown,
                "User Bash progress callback failed",
                bounded_redacted_presentation(error.what())));
        } catch (...) {
            active_user_bash_stop_source_.reset();
            user_bash_active_ = false;
            co_return std::unexpected(util::make_error(
                util::ErrorCode::Unknown,
                "User Bash progress callback failed"));
        }
    }
    UserBashOutputAccumulator output;
    try {
        shell_result = co_await services_.user_shell->execute(
            std::move(command),
            [recorded_command, exclude_from_context, &output, &progress_sink](
                std::string_view update) -> util::ExpectedVoid {
                output.append(update);
                if (!progress_sink) return {};
                try {
                    return progress_sink(UserBashProgress{
                        .command = recorded_command,
                        .output = output.tail(),
                        .exclude_from_context = exclude_from_context,
                    });
                } catch (const std::exception& error) {
                    return std::unexpected(util::make_error(
                        util::ErrorCode::Unknown,
                        "User Bash progress callback failed",
                        error.what()));
                } catch (...) {
                    return std::unexpected(util::make_error(
                        util::ErrorCode::Unknown,
                        "User Bash progress callback failed"));
                }
            },
            active_user_bash_stop_source_->get_token());
        if (shell_result) {
            output.finish();
        }
    } catch (const std::exception& error) {
        shell_result = std::unexpected(util::make_error(
            util::ErrorCode::Unknown,
            "User Shell execution failed",
            error.what()));
    } catch (...) {
        shell_result = std::unexpected(util::make_error(
            util::ErrorCode::Unknown,
            "User Shell execution failed",
            "unknown exception"));
    }

    active_user_bash_stop_source_.reset();
    user_bash_active_ = false;
    const auto finalize_if_last_active_work =
        [this]() -> boost::asio::awaitable<void> {
        if (lifecycle_ == Lifecycle::Closing && !prompt_active_) {
            co_await finalize_close_after_active_work();
        }
    };

    if (!shell_result) {
        output.discard();
        co_await finalize_if_last_active_work();
        co_return std::unexpected(std::move(shell_result.error()));
    }

    const auto artifact_error = output.artifact_error();
    auto message = make_bash_execution_message(
        *shell_result,
        output,
        recorded_command,
        exclude_from_context,
        completion_timestamp_ms());

    UserBashCompletion completion{
        .message = std::move(message),
        .diagnostic = artifact_error,
    };

    if (prompt_active_) {
        // Defer commitment until the whole Agent run, including steering and
        // follow-up continuations, settles: a Bash message must not split an
        // in-flight tool-call/tool-result sequence (pi recordBashResult
        // semantics). The completion timestamp above already records process
        // completion, not this deferral.
        auto pending = std::make_shared<PendingUserBashCommit>(PendingUserBashCommit{
            .completion = std::move(completion),
            .committed_signal = boost::asio::steady_timer(
                co_await boost::asio::this_coro::executor),
            .commit_result = {},
        });
        pending->committed_signal.expires_at(
            std::chrono::steady_clock::time_point::max());
        pending_user_bash_.push_back(pending);
        if (progress_sink) {
            try {
                (void)progress_sink(UserBashProgress{
                    .command = recorded_command,
                    .output = output.tail(),
                    .exclude_from_context = exclude_from_context,
                    .awaiting_commitment = true,
                    .exit_code = pending->completion.message.exit_code,
                    .cancelled = pending->completion.message.cancelled,
                    .truncated = pending->completion.message.truncated,
                    .full_output_path = pending->completion.message.full_output_path,
                });
            } catch (...) {
                // Execution already completed; a presentation failure must not
                // lose the pending commitment.
            }
        }
        boost::system::error_code wait_error;
        co_await pending->committed_signal.async_wait(
            boost::asio::redirect_error(boost::asio::use_awaitable, wait_error));
        if (!pending->commit_result) {
            co_return std::unexpected(pending->commit_result.error());
        }
        co_return std::move(pending->completion);
    }

    if (!agent_ || !session_.store) {
        co_await finalize_if_last_active_work();
        co_return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "session Agent is unavailable"));
    }
    if (auto committed = commit_user_bash_completion(completion); !committed) {
        co_await finalize_if_last_active_work();
        co_return std::unexpected(committed.error());
    }
    co_await finalize_if_last_active_work();
    co_return completion;
}

void AgentSessionRuntime::cancel_user_bash() {
    if (user_bash_active_ && active_user_bash_stop_source_) {
        (void)active_user_bash_stop_source_->request_stop();
    }
}

boost::asio::awaitable<util::ExpectedVoid> AgentSessionRuntime::run_agent_loop(
    ai::UserMessage prompt,
    std::stop_source stop_source) {
    if (!agent_) {
        co_return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "session Agent is unavailable"));
    }

    SessionEventCommitment commitment{*session_.store};
    std::optional<util::ExpectedVoid> result;
    result = co_await agent::detail::AgentPromptAccess::prompt(
        *agent_,
        std::move(prompt),
        commitment.sink(),
        stop_source);
    if (!result) {
        co_return commitment.conclude(std::move(result));
    }

    // T10 automatic compaction trigger policy (pi `_handlePostAgentRun` /
    // `_checkCompaction`): after a completed loop, an overflow terminal
    // compacts and retries the turn exactly once; a second overflow fails
    // with pi's verbatim recovery message; threshold compaction never
    // retries. The check re-runs after every retry exactly like pi's
    // `while (await this._handlePostAgentRun()) await this.agent.continue()`.
    for (;;) {
        const auto last_assistant =
            last_assistant_message_from(agent_->state().messages);
        if (!last_assistant) {
            break;
        }
        const auto outcome = co_await check_auto_compaction(
            *last_assistant, /*skip_aborted_check=*/true);
        if (outcome == AutoCompactionOutcome::OverflowRecoveryFailed) {
            // The run's messages (including the second overflow error) are
            // persisted through the commitment; the prompt fails with pi's
            // verbatim recovery message (the failure the `compaction_end`
            // event carries in pi).
            co_return commitment.conclude(std::optional<util::ExpectedVoid>{
                std::unexpected(util::make_error(
                    util::ErrorCode::Stream,
                    std::string{kOverflowRecoveryFailedMessage}))});
        }
        if (outcome != AutoCompactionOutcome::OverflowRetry) {
            break;
        }
        result = co_await agent::detail::AgentPromptAccess::continue_run(
            *agent_, commitment.sink(), stop_source);
        if (!result) {
            break;
        }
    }
    co_return commitment.conclude(std::move(result));
}

util::Expected<agent::AgentEventSubscription> AgentSessionRuntime::subscribe(
    agent::AgentEventSink sink) {
    if (auto rejected = reject_if_closed(); !rejected) {
        return std::unexpected(rejected.error());
    }
    if (!agent_) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "session is closed"));
    }
    return agent_->subscribe(std::move(sink));
}

util::ExpectedVoid AgentSessionRuntime::steer(
    std::string text,
    std::vector<ai::ImageContent> images,
    bool expand_prompt_templates) {
    if (auto rejected = reject_if_closed(); !rejected) {
        return rejected;
    }
    auto message = make_admitted_user_message(
        prompt_processor_, std::move(text), std::move(images), expand_prompt_templates);
    if (!message) {
        return std::unexpected(message.error());
    }
    return agent_->steer(ai::MessageVariant{std::move(*message)});
}

util::ExpectedVoid AgentSessionRuntime::follow_up(
    std::string text,
    std::vector<ai::ImageContent> images,
    bool expand_prompt_templates) {
    if (auto rejected = reject_if_closed(); !rejected) {
        return rejected;
    }
    auto message = make_admitted_user_message(
        prompt_processor_, std::move(text), std::move(images), expand_prompt_templates);
    if (!message) {
        return std::unexpected(message.error());
    }
    return agent_->follow_up(ai::MessageVariant{std::move(*message)});
}

util::ExpectedVoid AgentSessionRuntime::set_steering_mode(agent::InputQueueMode mode) {
    if (auto rejected = reject_if_closed(); !rejected) {
        return rejected;
    }
    return agent_ ? agent_->set_steering_mode(mode) : std::unexpected(util::make_error(
        util::ErrorCode::Validation, "session is closed"));
}

util::ExpectedVoid AgentSessionRuntime::set_follow_up_mode(agent::InputQueueMode mode) {
    if (auto rejected = reject_if_closed(); !rejected) {
        return rejected;
    }
    return agent_ ? agent_->set_follow_up_mode(mode) : std::unexpected(util::make_error(
        util::ErrorCode::Validation, "session is closed"));
}

util::ExpectedVoid AgentSessionRuntime::clear_steering_queue() {
    if (auto rejected = reject_if_closed(); !rejected) {
        return rejected;
    }
    return agent_ ? agent_->clear_steering_queue() : std::unexpected(util::make_error(
        util::ErrorCode::Validation, "session is closed"));
}

util::ExpectedVoid AgentSessionRuntime::clear_follow_up_queue() {
    if (auto rejected = reject_if_closed(); !rejected) {
        return rejected;
    }
    return agent_ ? agent_->clear_follow_up_queue() : std::unexpected(util::make_error(
        util::ErrorCode::Validation, "session is closed"));
}

util::ExpectedVoid AgentSessionRuntime::clear_input_queues() {
    if (auto rejected = reject_if_closed(); !rejected) {
        return rejected;
    }
    return agent_ ? agent_->clear_input_queues() : std::unexpected(util::make_error(
        util::ErrorCode::Validation, "session is closed"));
}

util::Expected<std::string> AgentSessionRuntime::set_thinking_level(
    std::string_view level) {
    if (auto rejected = reject_if_closed(); !rejected) {
        return std::unexpected(rejected.error());
    }
    if (!agent_) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation, "session is closed"));
    }

    const auto previous = agent_->state().thinking_level;
    auto effective = agent_->set_thinking_level(level);
    if (!effective) {
        return effective;
    }
    if (*effective == previous) {
        return effective;
    }

    // Persist the `thinking_level_change` session entry (pi
    // `appendThinkingLevelChange`). In-memory sessions have no v3 tree entry
    // surface and are not resumable, so the entry is skipped there exactly
    // like the creation-time `model_change` entry.
    if (auto* jsonl_store =
            dynamic_cast<harness::session::JsonlSessionStore*>(session_.store.get())) {
        if (auto appended = jsonl_store->append_thinking_level_change(
                std::nullopt, *effective);
            !appended) {
            return std::unexpected(std::move(appended.error()));
        }
    }

    // Persist the settings default unless the active model supports no
    // thinking and the level is "off" (pi agent-session.ts setThinkingLevel:
    // `if (this.supportsThinking() || effectiveLevel !== "off")`).
    const auto& active_model = agent_->state().model;
    if (services_.settings_manager &&
        (active_model.reasoning || *effective != "off")) {
        if (auto saved = services_.settings_manager->set_default_thinking_level(
                SettingsScope::Global, *effective);
            !saved) {
            return std::unexpected(std::move(saved.error()));
        }
    }
    return effective;
}

boost::asio::awaitable<util::Expected<coding_agent::CompactionResult>>
AgentSessionRuntime::compact(std::string custom_instructions) {
    if (auto rejected = reject_if_closed(); !rejected) {
        co_return std::unexpected(rejected.error());
    }
    if (compaction_active_) {
        co_return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "compaction is already in flight"));
    }

    // pi AgentSession.compact: abort the active run first, then wait for it
    // to settle before compacting (the run settles with the ordinary aborted
    // terminal; its assistant message is already committed to the store).
    if (prompt_active_ && active_stop_source_) {
        (void)active_stop_source_->request_stop();
        if (prompt_settled_signal_) {
            boost::system::error_code wait_error;
            co_await prompt_settled_signal_->async_wait(
                boost::asio::redirect_error(
                    boost::asio::use_awaitable, wait_error));
        }
    }

    compaction_active_ = true;
    util::Expected<coding_agent::CompactionResult> result;
    try {
        result = co_await compact_impl(std::move(custom_instructions));
    } catch (...) {
        const auto failure = prompt_exception(std::current_exception());
        result = std::unexpected(failure.error());
    }
    compaction_active_ = false;
    co_return result;
}

namespace {

[[nodiscard]] util::Error no_model_selected_error() {
    // pi formatNoModelSelectedMessage's login-help tail is Native TUI
    // presentation (auth-guidance); the C++ session carries only the core
    // directive. The placeholder kDefaultModel is the C++ "no model" state.
    return util::make_error(
        util::ErrorCode::Validation,
        "No model selected.\n\nThen use /model to select a model.");
}

} // namespace

boost::asio::awaitable<util::Expected<coding_agent::CompactionResult>>
AgentSessionRuntime::compact_impl(std::string custom_instructions) {
    if (!agent_ || !session_.store) {
        co_return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "session Agent is unavailable"));
    }
    const auto model = agent_->state().model;
    if (model.id == agent::detail::kDefaultModel.id) {
        co_return std::unexpected(no_model_selected_error());
    }
    auto* jsonl_store =
        dynamic_cast<harness::session::JsonlSessionStore*>(session_.store.get());
    if (jsonl_store == nullptr || !jsonl_store->path()) {
        // In-memory sessions have no tree/entry surface: there is no session
        // file to persist a CompactionEntry into or to rebuild context from.
        co_return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "compaction requires a persisted session file"));
    }
    const auto session_path = *jsonl_store->path();

    const auto settings = effective_compaction_settings();
    auto tree = harness::session::JsonlSessionStore::open_as_tree(session_path);
    if (!tree) {
        co_return std::unexpected(tree.error());
    }
    auto branch = tree->getBranch();
    // pi SessionManager.getBranch is root-to-leaf with the leaf last; the C++
    // tree walks leaf-to-root, so reverse for the machinery.
    std::reverse(branch.begin(), branch.end());

    auto preparation = harness::session::prepare_compaction(branch, settings);
    if (!preparation) {
        co_return std::unexpected(preparation.error());
    }
    if (!*preparation) {
        if (!branch.empty() &&
            branch.back()->kind == harness::session::SessionEntryKind::Compaction) {
            co_return std::unexpected(util::make_error(
                util::ErrorCode::Validation,
                "Already compacted"));
        }
        co_return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "Nothing to compact (session too small)"));
    }
    const auto& prep = **preparation;
    // pi AgentSession.compact refuses when neither history nor turn prefix has
    // messages to summarize (a session that fits the keepRecentTokens budget).
    if (prep.messages_to_summarize.empty() && prep.turn_prefix_messages.empty()) {
        co_return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "Nothing to compact (session too small)"));
    }

    co_return co_await execute_compaction(
        prep, settings, std::move(custom_instructions));
}

boost::asio::awaitable<util::Expected<coding_agent::CompactionResult>>
AgentSessionRuntime::execute_compaction(
    const harness::session::CompactionPreparation& preparation,
    const harness::session::CompactionSettings& settings,
    std::string custom_instructions) {
    if (!agent_ || !session_.store) {
        co_return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "session Agent is unavailable"));
    }
    auto* jsonl_store =
        dynamic_cast<harness::session::JsonlSessionStore*>(session_.store.get());
    if (jsonl_store == nullptr || !jsonl_store->path()) {
        // In-memory sessions have no tree/entry surface: there is no session
        // file to persist a CompactionEntry into or to rebuild context from.
        co_return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "compaction requires a persisted session file"));
    }
    const auto session_path = *jsonl_store->path();
    const auto model = agent_->state().model;

    harness::session::SummarizationStreamFn summarization_stream =
        [runtime = services_.model_runtime, model](
            ai::AiContext context,
            ai::SimpleStreamOptions options)
            -> boost::asio::awaitable<util::Expected<ai::AssistantMessage>> {
        if (!runtime) {
            co_return std::unexpected(util::make_error(
                util::ErrorCode::Validation,
                "model runtime is unavailable"));
        }
        co_return co_await runtime->stream_simple(
            model, std::move(context), std::move(options), {});
    };

    harness::session::CompactionRunOptions run_options;
    if (!custom_instructions.empty()) {
        run_options.custom_instructions = std::move(custom_instructions);
    }
    run_options.thinking_level = agent_->state().thinking_level;
    run_options.stop_token = std::stop_token{};
    run_options.summarization_stream = std::move(summarization_stream);

    auto result = co_await harness::session::compact(
        preparation, model, std::move(run_options));
    if (!result) {
        co_return std::unexpected(result.error());
    }

    // Persist the CompactionEntry with pi's full field set (summary,
    // firstKeptEntryId, tokensBefore, retainedTail, details, usage; fromHook
    // false for machinery-generated compactions).
    if (auto appended = jsonl_store->append_compaction(
            std::nullopt,
            result->summary,
            result->first_kept_entry_id,
            result->tokens_before,
            result->details,
            /*from_hook=*/false,
            result->retained_tail,
            result->usage);
        !appended) {
        co_return std::unexpected(appended.error());
    }

    // Rebuild the live context from the persisted tree exactly like pi
    // (`this.agent.state.messages = sessionContext.messages`): the next
    // prompt's model context is compactionSummary + retained tail.
    auto rebuilt = harness::session::JsonlSessionStore::open_as_tree(session_path);
    if (!rebuilt) {
        co_return std::unexpected(rebuilt.error());
    }
    auto context = rebuilt->buildSessionContext();
    if (auto replaced = agent::detail::AgentMessageAccess::replace_messages(
            *agent_, context.messages);
        !replaced) {
        co_return std::unexpected(replaced.error());
    }

    coding_agent::CompactionResult compaction_result;
    compaction_result.summary = std::move(result->summary);
    compaction_result.first_kept_entry_id =
        std::move(result->first_kept_entry_id);
    compaction_result.tokens_before = result->tokens_before;
    std::size_t estimated_after = 0;
    for (const auto& message : context.messages) {
        estimated_after += harness::session::estimate_tokens(message);
    }
    compaction_result.estimated_tokens_after = estimated_after;
    compaction_result.usage = result->usage;
    compaction_result.details = result->details;
    co_return compaction_result;
}

boost::asio::awaitable<bool> AgentSessionRuntime::run_auto_compaction(
    bool will_retry) {
    const auto settings = effective_compaction_settings();
    if (!agent_ || !session_.store) {
        co_return false;
    }
    const auto model = agent_->state().model;
    if (model.id == agent::detail::kDefaultModel.id) {
        co_return false;
    }
    auto* jsonl_store =
        dynamic_cast<harness::session::JsonlSessionStore*>(session_.store.get());
    if (jsonl_store == nullptr || !jsonl_store->path()) {
        // In-memory sessions have no tree/entry surface (same confinement as
        // the manual trigger): auto-compaction is skipped silently.
        co_return false;
    }
    const auto session_path = *jsonl_store->path();

    auto tree = harness::session::JsonlSessionStore::open_as_tree(session_path);
    if (!tree) {
        co_return false;
    }
    auto branch = tree->getBranch();
    std::reverse(branch.begin(), branch.end());

    auto preparation = harness::session::prepare_compaction(branch, settings);
    if (!preparation || !*preparation) {
        co_return false;
    }
    const auto& prep = **preparation;
    // pi's coding-agent `prepareCompaction` returns undefined when there is
    // nothing to summarize; the same guard skips the auto trigger.
    if (prep.messages_to_summarize.empty() && prep.turn_prefix_messages.empty()) {
        co_return false;
    }

    auto result = co_await execute_compaction(prep, settings, {});
    if (!result) {
        co_return false;
    }

    if (will_retry) {
        // The rebuilt context can still end in the failed error assistant
        // message (it lives in the retained tail); drop it so the
        // continuation's last message is the user prompt (pi
        // `_runAutoCompaction` willRetry branch).
        if (auto popped =
                agent::detail::AgentMessageAccess::pop_trailing_assistant(*agent_);
            !popped) {
            co_return false;
        }
    }
    co_return will_retry;
}

boost::asio::awaitable<AgentSessionRuntime::AutoCompactionOutcome>
AgentSessionRuntime::check_auto_compaction(
    const ai::AssistantMessage& assistant_message,
    bool skip_aborted_check) {
    const auto settings = effective_compaction_settings();
    if (!settings.enabled) {
        co_return AutoCompactionOutcome::None;
    }
    if (skip_aborted_check &&
        assistant_message.stop_reason == ai::AssistantStopReason::Aborted) {
        co_return AutoCompactionOutcome::None;
    }

    const auto& model = agent_->state().model;
    const std::size_t context_window =
        static_cast<std::size_t>(model.context_window);
    const bool same_model =
        model.id == assistant_message.model &&
        model.provider == assistant_message.provider;

    // Skip compaction checks when the assistant message predates the latest
    // compaction boundary: a stale pre-compaction usage/error must not
    // retrigger compaction on the first prompt after compaction (pi
    // `_checkCompaction` `assistantIsFromBeforeCompaction`).
    const auto compaction_timestamp = latest_compaction_timestamp();
    if (compaction_timestamp &&
        assistant_message.timestamp <= *compaction_timestamp) {
        co_return AutoCompactionOutcome::None;
    }

    // Case 1: Overflow. An error terminal (or a successful response whose
    // usage already exceeds the window) compacts; only the error terminal
    // retries, because continue() cannot continue from a completed assistant
    // message. Overflow never routes to turn auto-retry: this branch consumes
    // it before any retry policy (T12's boundary).
    if (same_model && harness::session::is_context_overflow(
                          assistant_message, context_window)) {
        const bool will_retry =
            assistant_message.stop_reason != ai::AssistantStopReason::Stop;
        if (!will_retry) {
            if (co_await run_auto_compaction(false)) {
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
        if (auto popped =
                agent::detail::AgentMessageAccess::pop_trailing_assistant(*agent_);
            !popped) {
            co_return AutoCompactionOutcome::None;
        }
        if (co_await run_auto_compaction(true)) {
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
    const std::size_t direct_context_tokens =
        harness::session::calculate_context_tokens(assistant_message.usage);
    std::size_t context_tokens = direct_context_tokens;
    if (assistant_message.stop_reason == ai::AssistantStopReason::Error ||
        direct_context_tokens == 0) {
        const auto estimate =
            harness::session::estimate_context_tokens(agent_->state().messages);
        if (!estimate.last_usage_index) {
            // No usage data at all: nothing to base a threshold decision on.
            co_return AutoCompactionOutcome::None;
        }
        if (compaction_timestamp) {
            // The usage source must be post-compaction: kept pre-compaction
            // messages carry stale usage reflecting the old (larger) context
            // and would falsely trigger compaction right after one finished.
            const auto& usage_message =
                agent_->state().messages[*estimate.last_usage_index];
            const auto* usage_assistant =
                std::get_if<ai::AssistantMessage>(&usage_message);
            if (usage_assistant != nullptr &&
                usage_assistant->timestamp <= *compaction_timestamp) {
                co_return AutoCompactionOutcome::None;
            }
        }
        context_tokens = estimate.tokens;
    }
    if (harness::session::should_compact(
            context_tokens, context_window, settings)) {
        if (co_await run_auto_compaction(false)) {
            co_return AutoCompactionOutcome::Compacted;
        }
    }
    co_return AutoCompactionOutcome::None;
}

harness::session::CompactionSettings
AgentSessionRuntime::effective_compaction_settings() const {
    harness::session::CompactionSettings settings =
        harness::session::kDefaultCompactionSettings;
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
        settings.reserve_tokens =
            static_cast<std::size_t>(*configured->reserve_tokens);
    }
    if (configured->keep_recent_tokens) {
        settings.keep_recent_tokens =
            static_cast<std::size_t>(*configured->keep_recent_tokens);
    }
    return settings;
}

std::optional<ai::TimestampMs>
AgentSessionRuntime::latest_compaction_timestamp() const {
    auto* jsonl_store =
        dynamic_cast<harness::session::JsonlSessionStore*>(session_.store.get());
    if (jsonl_store == nullptr || !jsonl_store->path()) {
        return std::nullopt;
    }
    auto tree =
        harness::session::JsonlSessionStore::open_as_tree(*jsonl_store->path());
    if (!tree) {
        return std::nullopt;
    }
    // getBranch is leaf-to-root; the first compaction encountered is the
    // latest on the active path (pi `getLatestCompactionEntry`).
    for (const auto* entry : tree->getBranch()) {
        if (entry->kind == harness::session::SessionEntryKind::Compaction) {
            return entry->timestamp;
        }
    }
    return std::nullopt;
}

AgentSessionSnapshot AgentSessionRuntime::snapshot(
    const std::optional<std::filesystem::path>& session_path) const {
    return AgentSessionSnapshot{
        .agent_state = agent_ ? agent_->state() : agent::AgentState{},
        .metadata = session_.metadata,
        .topology = session_.topology,
        .session_path = session_path,
    };
}

std::size_t AgentSessionRuntime::message_count() const {
    return agent_ ? agent_->state().messages.size() : 0;
}

std::optional<std::string> AgentSessionRuntime::last_assistant_text() const {
    return agent_ ? last_assistant_text_from(agent_->state().messages) : std::nullopt;
}

const std::vector<Skill>& AgentSessionRuntime::skills() const {
    static const std::vector<Skill> kEmptySkills;
    return prompt_processor_ ? prompt_processor_->skills() : kEmptySkills;
}

const std::vector<PromptTemplate>& AgentSessionRuntime::templates() const {
    static const std::vector<PromptTemplate> kEmptyTemplates;
    return prompt_processor_ ? prompt_processor_->templates() : kEmptyTemplates;
}

void AgentSessionRuntime::abort() {
    if (prompt_active_ && active_stop_source_) {
        (void)active_stop_source_->request_stop();
    }
}

void AgentSessionRuntime::close() noexcept {
    if (lifecycle_ != Lifecycle::Open) {
        return;
    }
    lifecycle_ = Lifecycle::Closing;

    // Request work-scoped cancellation but retain the active loop, callbacks,
    // commitment, store, and capabilities until each active operation unwinds
    // through its ordinary lifecycle. The last active work to settle
    // finalizes the close.
    if (prompt_active_ && active_stop_source_) {
        (void)active_stop_source_->request_stop();
    }
    if (user_bash_active_ && active_user_bash_stop_source_) {
        (void)active_user_bash_stop_source_->request_stop();
    }
    if (!prompt_active_ && !user_bash_active_) {
        finalize_close();
    }
}

std::shared_ptr<harness::AsyncExecutionEnv>
AgentSessionRuntime::release_close_resources() noexcept {
    if (agent_) {
        agent_->clear_subscriptions();
    }
    agent_.reset();
    prompt_processor_.reset();
    session_.store.reset();
    services_.user_shell.reset();
    if (services_.model_runtime_owned) {
        services_.model_runtime.reset();
    }

    if (services_.env_owned) {
        return std::move(services_.env);
    }
    services_.env.reset();
    return {};
}

boost::asio::awaitable<void> AgentSessionRuntime::finalize_close_after_active_work() {
    auto owned_env = release_close_resources();
    if (owned_env) {
        try {
            co_await owned_env->cleanup();
        } catch (...) {
            // cleanup() is best-effort and must not make close fallible.
        }
    }
    lifecycle_ = Lifecycle::Closed;
}

void AgentSessionRuntime::finalize_close() noexcept {
    if (lifecycle_ == Lifecycle::Closed) {
        return;
    }
    auto owned_env = release_close_resources();
    lifecycle_ = Lifecycle::Closed;

    // Idle close has no host executor to await. Transfer the factory-owned
    // environment to a posted best-effort cleanup task; host-owned environments
    // were detached above without cleanup().
    if (owned_env) {
        try {
            // post() prevents a cleanup coroutine from executing inline on the
            // close() stack before its first suspension point.
            boost::asio::post(
                boost::asio::system_executor{},
                [env = std::move(owned_env)]() mutable {
                    try {
                        boost::asio::co_spawn(
                            boost::asio::system_executor{},
                            [env = std::move(env)]() -> boost::asio::awaitable<void> {
                                try {
                                    co_await env->cleanup();
                                } catch (...) {
                                    // cleanup() is best-effort and must not make close fallible.
                                }
                            },
                            boost::asio::detached);
                    } catch (...) {
                        // Launch remains best-effort after close has released ownership.
                    }
                });
        } catch (...) {
            // Scheduling is also best-effort; close remains noexcept.
        }
    }
}

} // namespace cch::coding_agent::runtime

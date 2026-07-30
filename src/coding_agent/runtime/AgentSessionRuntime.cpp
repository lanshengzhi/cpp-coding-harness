#include "AgentSessionRuntime.hpp"

#include <cch/ai/Content.hpp>

#include "agent/AgentMessageAccess.hpp"
#include "agent/AgentPromptAccess.hpp"
#include "coding_agent/BoundedText.hpp"
#include "coding_agent/SkillFormatting.hpp"
#include "coding_agent/runtime/UserBashOutputAccumulator.hpp"
#include "util/TerminalText.hpp"

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

[[nodiscard]] std::string normalize_terminal_text(std::string_view text) {
    const auto stripped = util::strip_terminal_escape_sequences(text);
    std::string normalized;
    normalized.reserve(stripped.size());
    for (std::size_t index = 0; index < stripped.size();) {
        const auto value = static_cast<unsigned char>(stripped[index]);
        if (stripped[index] == '\r') {
            normalized.push_back('\n');
            index += index + 1 < stripped.size() && stripped[index + 1] == '\n' ? 2 : 1;
            continue;
        }
        if (value < 0x20 && stripped[index] != '\n' && stripped[index] != '\t') {
            ++index;
            continue;
        }
        normalized.push_back(stripped[index++]);
    }
    return normalized;
}

[[nodiscard]] std::string safe_user_bash_command(std::string command) {
    return bounded_redacted_presentation(normalize_terminal_text(command));
}

[[nodiscard]] util::Error safe_user_bash_error(util::Error error) {
    return util::make_error(
        error.code,
        bounded_redacted_presentation(std::move(error.message)),
        bounded_redacted_presentation(std::move(error.detail)),
        error.context
            ? std::optional<std::string>{bounded_redacted_presentation(std::move(*error.context))}
            : std::nullopt);
}

[[nodiscard]] ai::TimestampMs completion_timestamp_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

[[nodiscard]] ai::BashExecutionMessage make_bash_execution_message(
    const UserShellResult& result,
    const UserBashOutputAccumulator& output,
    std::string safe_command,
    bool exclude_from_context,
    ai::TimestampMs timestamp) {
    ai::BashExecutionMessage message;
    message.command = std::move(safe_command);
    // The bounded sanitized tail is the model-context value; the spill path
    // is recorded alongside it, never substituted for it.
    message.output = output.tail();
    message.exit_code = result.cancelled ? std::nullopt : result.exit_code;
    message.cancelled = result.cancelled;
    message.truncated = output.truncated();
    if (output.full_output_path()) {
        message.full_output_path = bounded_redacted_presentation(
            normalize_terminal_text(*output.full_output_path()));
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
    options.model = ai::Model{std::move(config_.model)};
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
    initial_state.thinking_level = session_.context_thinking_level.value_or("off");

    // Construct Agent last: it borrows the factory-owned client and takes sole
    // ownership of the move-only tool registry.
    agent_.emplace(
        *services_.client,
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
        return std::unexpected(safe_user_bash_error(committed.error()));
    }
    if (auto persisted = session_.store->append(
            ai::MessageVariant{completion.message});
        !persisted) {
        completion.diagnostic = safe_user_bash_error(persisted.error());
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

    util::ExpectedVoid result;
    try {
        bool run_agent = true;
        std::string expanded_prompt;
        if (!prompt_processor_) {
            result = std::unexpected(util::make_error(
                util::ErrorCode::Validation,
                "session is closed"));
            run_agent = false;
        } else {
            auto expanded = prompt_processor_->process(
                std::move(prompt), expand_prompt_templates);
            expanded_prompt = std::move(expanded.text);
        }

        if (run_agent && on_preflight_accepted) {
            if (auto acknowledged = on_preflight_accepted(); !acknowledged) {
                result = std::unexpected(acknowledged.error());
                run_agent = false;
            }
        }
        if (run_agent) {
            auto user_message = ai::user_text_message(std::move(expanded_prompt));
            user_message.content.reserve(user_message.content.size() + images.size());
            for (auto& image : images) {
                user_message.content.emplace_back(std::move(image));
            }
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
    const auto safe_command = safe_user_bash_command(command);
    util::Expected<UserShellResult> shell_result = std::unexpected(util::make_error(
        util::ErrorCode::Unknown,
        "User Shell execution did not finish"));
    if (progress_sink) {
        try {
            if (auto started = progress_sink(UserBashProgress{
                    .command = safe_command,
                    .output = {},
                    .exclude_from_context = exclude_from_context,
                });
                !started) {
                active_user_bash_stop_source_.reset();
                user_bash_active_ = false;
                co_return std::unexpected(safe_user_bash_error(started.error()));
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
            [safe_command, exclude_from_context, &output, &progress_sink](
                std::string_view update) -> util::ExpectedVoid {
                output.append(update);
                if (!progress_sink) return {};
                try {
                    return progress_sink(UserBashProgress{
                        .command = safe_command,
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
        co_return std::unexpected(safe_user_bash_error(shell_result.error()));
    }

    const auto artifact_error = output.artifact_error();
    auto message = make_bash_execution_message(
        *shell_result,
        output,
        safe_command,
        exclude_from_context,
        completion_timestamp_ms());

    UserBashCompletion completion{
        .message = std::move(message),
        .diagnostic = artifact_error
            ? std::optional<util::Error>{safe_user_bash_error(*artifact_error)}
            : std::nullopt,
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
                    .command = safe_command,
                    .output = output.tail(),
                    .exclude_from_context = exclude_from_context,
                    .awaiting_commitment = true,
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
        std::move(stop_source));
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

namespace {

[[nodiscard]] util::Expected<ai::UserMessage> make_queued_user_message(
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
    message.content.reserve(message.content.size() + images.size());
    for (auto& image : images) {
        message.content.emplace_back(std::move(image));
    }
    return message;
}

} // namespace

util::ExpectedVoid AgentSessionRuntime::steer(
    std::string text,
    std::vector<ai::ImageContent> images,
    bool expand_prompt_templates) {
    if (auto rejected = reject_if_closed(); !rejected) {
        return rejected;
    }
    auto message = make_queued_user_message(
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
    auto message = make_queued_user_message(
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
    services_.client.reset();
    services_.user_shell.reset();

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

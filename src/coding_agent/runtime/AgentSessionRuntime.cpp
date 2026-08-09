#include "AgentSessionRuntime.hpp"

#include <cch/ai/Content.hpp>
#include <cch/coding_agent/AuthGuidance.hpp>
#include <cch/coding_agent/Settings.hpp>
#include <cch/harness/session/JsonlSessionStore.hpp>

#include "agent/AgentMessageAccess.hpp"
#include "agent/AgentPromptAccess.hpp"
#include "ai/ModelThinkingLevel.hpp"
#include "ai/utils/RetryClassifier.hpp"
#include "coding_agent/BoundedText.hpp"
#include "coding_agent/SkillFormatting.hpp"
#include "coding_agent/prompt/PromptExpansion.hpp"
#include "coding_agent/prompt/PromptTemplateExpander.hpp"
#include "coding_agent/prompt/SystemPromptBuilder.hpp"
#include "coding_agent/runtime/AuthGuidanceStreamRuntime.hpp"
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
#include <array>
#include <chrono>
#include <cstdint>
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
    std::vector<Skill> skills,
    std::vector<PromptTemplate> templates,
    AgentSessionRuntimeConfig config)
    : services_(std::move(services)),
      session_(std::move(session)),
      skills_(std::move(skills)),
      templates_(std::move(templates)),
      config_(std::move(config)) {
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
    options.convert_to_llm = [](
                                  std::vector<ai::MessageVariant> messages)
        -> boost::asio::awaitable<
            util::Expected<std::vector<ai::MessageVariant>>> {
        std::erase_if(messages, [](const ai::MessageVariant& message) {
            const auto* bash = std::get_if<ai::BashExecutionMessage>(&message);
            return bash != nullptr && bash->exclude_from_context;
        });
        co_return messages;
    };
    // The System Prompt is built once at session construction in pi's exact
    // shape (ADR 0036 G4; `core/agent-session.ts` `_rebuildSystemPrompt` +
    // `core/system-prompt.ts` `buildSystemPrompt`) and flows into every run
    // through `AgentContext.system_prompt`, exactly like pi's
    // `agent.state.systemPrompt`. The resource loader's P20 inputs land
    // here: the custom prompt (`--system-prompt` / SYSTEM.md), the append
    // strings joined with `"\n\n"` (`--append-system-prompt` /
    // APPEND_SYSTEM.md), and the Project Context Files (never trust-gated).
    prompt::BuildSystemPromptOptions prompt_options;
    prompt_options.customPrompt = config_.custom_prompt;
    // pi `_rebuildSystemPrompt`: append strings join with `"\n\n"`; an
    // empty list appends nothing.
    if (!config_.append_system_prompt.empty()) {
        std::string joined = config_.append_system_prompt.front();
        for (std::size_t index = 1; index < config_.append_system_prompt.size();
             ++index) {
            joined += "\n\n";
            joined += config_.append_system_prompt[index];
        }
        prompt_options.appendSystemPrompt = std::move(joined);
    }
    prompt_options.contextFiles = config_.context_files;
    // pi `_refreshToolRegistry` → `_toolPromptSnippets`/`_toolPromptGuidelines`:
    // prompt metadata for the active tools, collected before the registry
    // moves into the Agent below. pi's default active tool order
    // `["read", "bash", "edit", "write"]` is kept for the rendered list.
    static constexpr std::array kFixedToolNames{"read", "bash", "edit", "write"};
    std::vector<std::string> selected_tools;
    for (const char* name : kFixedToolNames) {
        auto metadata = services_.tools.prompt_metadata(name);
        if (!metadata) {
            continue;
        }
        selected_tools.emplace_back(name);
        if (metadata->snippet) {
            prompt_options.toolSnippets.emplace(name, *metadata->snippet);
        }
        prompt_options.promptGuidelines.insert(
            prompt_options.promptGuidelines.end(),
            metadata->guidelines.begin(),
            metadata->guidelines.end());
    }
    prompt_options.selectedTools = std::move(selected_tools);
    prompt_options.cwd = session_.workspace.string();
    prompt_options.skills = skills_;
    // Identity delta: the C++ binary's own documentation paths (pi
    // `config.ts` `getReadmePath`/`getDocsPath`/`getExamplesPath` resolve the
    // pi package; cch resolves its own source tree).
#ifndef CCH_SOURCE_DIR
    constexpr std::string_view kSourceDir = "";
#else
    constexpr std::string_view kSourceDir = CCH_SOURCE_DIR;
#endif
    prompt_options.readmePath = std::string{kSourceDir} + "/README.md";
    prompt_options.docsPath = std::string{kSourceDir} + "/docs";
    prompt_options.examplesPath = std::string{kSourceDir} + "/examples";
    options.system_prompt = buildSystemPrompt(prompt_options);

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

    // Request-time re-auth guidance (pi `_getRequiredRequestAuth`): the
    // Agent's stream runs through a session-layer decorator that rewrites
    // auth/oauth-category terminal failures to pi's two verbatim guidance
    // branches; the same decorator serves the summarization seam below.
    auth_guided_runtime_ = std::make_shared<AuthGuidanceStreamRuntime>(
        services_.model_runtime);

    // Construct Agent last: it holds the factory-owned ModelRuntime (the sole
    // injectable seam per #326) and takes sole ownership of the move-only tool
    // registry.
    agent_.emplace(
        auth_guided_runtime_,
        std::move(services_.tools),
        std::move(options),
        std::move(initial_state));

    // Expose the live session facts to the model Bash Tool (pi
    // `resolveSpawnContext`); the Agent's clamped state is authoritative.
    refresh_bash_session_environment();
}

util::ExpectedVoid AgentSessionRuntime::reject_if_closed() const {
    if (lifecycle_ != Lifecycle::Open) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "session is closed"));
    }
    return {};
}

void AgentSessionRuntime::refresh_bash_session_environment() {
    if (!services_.bash_session_environment || !agent_) {
        return;
    }
    auto& session_environment = *services_.bash_session_environment;
    // pi `resolveSpawnContext`: `getSessionId()` always; `getSessionFile()`
    // only for persisted sessions; `ctx.model`/`ctx.thinkingLevel` from the
    // Agent's live (clamped) state.
    session_environment.session_id = session_.metadata.session_id;
    if (session_.store) {
        if (auto path = session_.store->path()) {
            session_environment.session_file = path->string();
        }
    }
    const auto state = agent_->state();
    session_environment.provider = state.model.provider;
    session_environment.model = state.model.id;
    session_environment.reasoning_level =
        state.thinking_level.empty()
            ? std::nullopt
            : std::optional<std::string>{state.thinking_level};
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
/// follow-up): optional skill/prompt-template expansion, then image content
/// appended to one complete user Agent Message.
[[nodiscard]] ai::UserMessage make_admitted_user_message(
    std::string text,
    const std::vector<Skill>& skills,
    const std::vector<PromptTemplate>& templates,
    std::vector<ai::ImageContent> images,
    bool expand_prompt_templates) {
    auto expanded = prompt::expand_prompt_input(
        std::move(text), skills, templates, expand_prompt_templates);
    auto message = ai::user_text_message(std::move(expanded));
    auto& blocks = std::get<std::vector<ai::Content>>(message.content);
    blocks.reserve(blocks.size() + images.size());
    for (auto& image : images) {
        blocks.emplace_back(std::move(image));
    }
    return message;
}

} // namespace

boost::asio::awaitable<util::ExpectedVoid>
AgentSessionRuntime::preflight_auth_guidance() {
    if (!agent_ || !services_.model_runtime) {
        co_return util::ExpectedVoid{};
    }
    const auto& model = agent_->state().model;
    // The placeholder kDefaultModel is the C++ "no model" state; "no model"
    // is not an auth failure, and streaming it fails through normal provider
    // lookup ("Unknown provider: unknown") exactly like pi.
    if (model.id == agent::detail::kDefaultModel.id) {
        co_return util::ExpectedVoid{};
    }
    // pi `prompt()`: `hasConfiguredAuth(provider) ||
    // (await checkAuth(provider)) !== undefined`. The live `checkAuth` is the
    // authoritative backstop (the snapshot may be stale); it is
    // side-effect-free and never refreshes OAuth.
    if (services_.model_runtime->has_configured_auth(model.provider)) {
        co_return util::ExpectedVoid{};
    }
    auto checked = co_await services_.model_runtime->check_auth(model.provider);
    if (!checked) {
        co_return std::unexpected(std::move(checked.error()));
    }
    if (*checked) {
        co_return util::ExpectedVoid{};
    }
    const std::string provider{model.provider};
    if (services_.model_runtime->is_using_oauth(provider)) {
        co_return std::unexpected(util::make_error(
            util::ErrorCode::Auth,
            format_oauth_reauthenticate_message(provider)));
    }
    co_return std::unexpected(util::make_error(
        util::ErrorCode::Auth,
        format_no_api_key_found_message(
            provider,
            std::filesystem::path{kDefaultAuthGuidanceDocsPath})));
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
    // A concurrent manual compaction awaits this signal after requesting run
    // cancellation; it is cancelled exactly when the run settles (the same
    // waiter-before-cancel ordering as PendingUserBashCommit).
    prompt_settled_signal_.emplace(co_await boost::asio::this_coro::executor);
    prompt_settled_signal_->expires_at(
        std::chrono::steady_clock::time_point::max());

    util::ExpectedVoid result;
    try {
        ai::UserMessage user_message = make_admitted_user_message(
            std::move(prompt),
            skills_,
            templates_,
            std::move(images),
            expand_prompt_templates);

        // pi `prompt()` auth preflight: a real model whose provider has no
        // configured auth fails with pi's verbatim re-auth guidance
        // before the run starts (the `kDefaultModel` placeholder is
        // skipped and keeps its ordinary "Unknown provider: unknown"
        // streaming failure).
        bool run_agent = true;
        if (auto admitted = co_await preflight_auth_guidance();
            !admitted) {
            result = std::unexpected(admitted.error());
            run_agent = false;
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
    // The commitment sink also observes assistant message endings so the turn
    // auto-retry success event fires at the first non-error assistant message
    // (pi `_handleAgentEvent` message_end handler resets `_retryAttempt` and
    // emits `auto_retry_end success`). Rebuilt per call because the wrapped
    // sink is move-only and each prompt/continue takes it by value.
    const auto make_retry_observing_sink = [&]() {
        return agent::AgentEventCommitter{
            [this, inner = commitment.sink()](
                const agent::AgentLifecycleEvent& event) mutable
                -> util::ExpectedVoid {
                if (retry_attempt_ > 0) {
                    if (const auto* end =
                            std::get_if<agent::MessageEndEvent>(&event)) {
                        const auto* assistant =
                            std::get_if<ai::AssistantMessage>(&end->message);
                        if (assistant != nullptr &&
                            assistant->stop_reason !=
                                ai::AssistantStopReason::Error) {
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

    std::optional<util::ExpectedVoid> result;
    result = co_await agent::detail::AgentPromptAccess::prompt(
        *agent_,
        std::move(prompt),
        make_retry_observing_sink(),
        stop_source);
    if (!result) {
        co_return commitment.conclude(std::move(result));
    }

    // Post-run loop in pi `_handlePostAgentRun` order: turn auto-retry (T12)
    // first, then the automatic compaction trigger (T10). Overflow errors are
    // never retryable (`is_retryable_error` excludes them), so the two
    // recovery paths never interfere: overflow routes to compact-and-retry
    // exactly once, while transient provider/network errors retry with
    // exponential backoff through the agent continuation mechanism.
    for (;;) {
        const auto last_assistant =
            last_assistant_message_from(agent_->state().messages);
        if (!last_assistant) {
            break;
        }

        if (is_retryable_error(*last_assistant)) {
            if (co_await prepare_retry(
                    *last_assistant, stop_source.get_token())) {
                result =
                    co_await agent::detail::AgentPromptAccess::continue_run(
                        *agent_, make_retry_observing_sink(), stop_source);
                if (!result) {
                    break;
                }
                continue;
            }
        }
        if (last_assistant->stop_reason == ai::AssistantStopReason::Error &&
            retry_attempt_ > 0) {
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
            *agent_, make_retry_observing_sink(), stop_source);
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

// ── Session-event subscriptions (pi `AgentSessionEvent`) ────────────────────

} // namespace cch::coding_agent::runtime

namespace cch::coding_agent {

struct SessionEventSubscription::Impl {
    std::size_t id{0};
    std::weak_ptr<runtime::AgentSessionRuntime::SessionSubscriptionAnchor> anchor;
};

SessionEventSubscription::SessionEventSubscription(
    SessionEventSubscription&&) noexcept = default;
SessionEventSubscription& SessionEventSubscription::operator=(
    SessionEventSubscription&& other) noexcept {
    if (this != &other) {
        unsubscribe();
        impl_ = std::move(other.impl_);
    }
    return *this;
}
SessionEventSubscription::~SessionEventSubscription() {
    unsubscribe();
}

void SessionEventSubscription::unsubscribe() {
    if (!impl_) {
        return;
    }
    const auto anchor = impl_->anchor.lock();
    const auto id = impl_->id;
    impl_.reset();
    if (anchor && anchor->runtime) {
        auto& observers = anchor->runtime->session_event_observers_;
        // Mark-only so an unsubscribe from inside an observer callback cannot
        // invalidate the delivery loop; `emit_session_event` erases
        // unregistered observers after delivery (the Agent's
        // `remove_unregistered_subscribers` pattern).
        for (const auto& observer : observers) {
            if (observer->id == id) {
                observer->registered = false;
                observer->delivery_enabled = false;
                break;
            }
        }
    }
}

SessionEventSubscription::operator bool() const {
    if (!impl_) {
        return false;
    }
    const auto anchor = impl_->anchor.lock();
    if (!anchor || !anchor->runtime) {
        return false;
    }
    for (const auto& observer : anchor->runtime->session_event_observers_) {
        if (observer->id == impl_->id && observer->registered) {
            return true;
        }
    }
    return false;
}

} // namespace cch::coding_agent

namespace cch::coding_agent::runtime {

util::Expected<SessionEventSubscription>
AgentSessionRuntime::subscribe_session(AgentSessionEventSink sink) {
    if (auto rejected = reject_if_closed(); !rejected) {
        return std::unexpected(rejected.error());
    }
    if (!sink) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "session event sink is empty"));
    }
    auto subscriber = std::make_shared<SessionSubscriber>(SessionSubscriber{
        .id = next_session_subscriber_id_++,
        .sink = std::move(sink),
    });
    const auto id = subscriber->id;
    session_event_observers_.push_back(std::move(subscriber));

    SessionEventSubscription subscription;
    subscription.impl_ = std::make_unique<SessionEventSubscription::Impl>(
        SessionEventSubscription::Impl{
            .id = id,
            .anchor = session_event_anchor_,
        });
    return subscription;
}

void AgentSessionRuntime::emit_session_event(const AgentSessionEvent& event) {
    if (session_event_observers_.empty()) {
        return;
    }
    for (const auto& subscriber : session_event_observers_) {
        if (!subscriber->delivery_enabled || !subscriber->sink) {
            continue;
        }
        try {
            if (auto observed = subscriber->sink(event); !observed) {
                // A failing observer is deactivated and never vetoes retry
                // progress or persistence (ADR 0017). The runtime has no
                // diagnostics surface for session events yet; the Agent's
                // observer diagnostics channel covers lifecycle observers.
                subscriber->registered = false;
                subscriber->delivery_enabled = false;
            }
        } catch (const std::exception&) {
            subscriber->registered = false;
            subscriber->delivery_enabled = false;
        } catch (...) {
            subscriber->registered = false;
            subscriber->delivery_enabled = false;
        }
    }
    std::erase_if(
        session_event_observers_,
        [](const std::shared_ptr<SessionSubscriber>& subscriber) {
            return !subscriber->registered;
        });
}

util::ExpectedVoid AgentSessionRuntime::steer(
    std::string text,
    std::vector<ai::ImageContent> images,
    bool expand_prompt_templates) {
    if (auto rejected = reject_if_closed(); !rejected) {
        return rejected;
    }
    auto message = make_admitted_user_message(
        std::move(text), skills_, templates_, std::move(images), expand_prompt_templates);
    return agent_->steer(ai::MessageVariant{std::move(message)});
}

util::ExpectedVoid AgentSessionRuntime::follow_up(
    std::string text,
    std::vector<ai::ImageContent> images,
    bool expand_prompt_templates) {
    if (auto rejected = reject_if_closed(); !rejected) {
        return rejected;
    }
    auto message = make_admitted_user_message(
        std::move(text), skills_, templates_, std::move(images), expand_prompt_templates);
    return agent_->follow_up(ai::MessageVariant{std::move(message)});
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
    // The model Bash Tool reads the live thinking level at execution time.
    refresh_bash_session_environment();

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

boost::asio::awaitable<util::ExpectedVoid> AgentSessionRuntime::set_model(
    ai::Model model) {
    if (auto rejected = reject_if_closed(); !rejected) {
        co_return std::unexpected(rejected.error());
    }
    if (!agent_ || !services_.model_runtime) {
        co_return std::unexpected(util::make_error(
            util::ErrorCode::Validation, "session is closed"));
    }

    // pi setModel: `if (!(await checkAuth(model.provider))) throw`.
    auto checked = co_await services_.model_runtime->check_auth(model.provider);
    if (!checked) {
        co_return std::unexpected(std::move(checked.error()));
    }
    if (!*checked) {
        co_return std::unexpected(util::make_error(
            util::ErrorCode::Auth,
            "No API key for " + model.provider + "/" + model.id));
    }

    // pi `_getThinkingLevelForModelSwitch`: a current model without thinking
    // support falls back to the merged settings default (then pi's
    // DEFAULT_THINKING_LEVEL); otherwise the current level is kept and
    // re-clamped against the new model below.
    const auto thinking_level = resolve_thinking_level_for_switch(std::nullopt);
    co_return co_await apply_model_switch(std::move(model), thinking_level);
}

/// pi `_getThinkingLevelForModelSwitch`: an explicit scoped-model level wins;
/// otherwise a current model without thinking support falls back to the
/// merged settings default (then pi's DEFAULT_THINKING_LEVEL); otherwise the
/// current level is kept and re-clamped against the new model below.
[[nodiscard]] std::string AgentSessionRuntime::resolve_thinking_level_for_switch(
    const std::optional<std::string>& explicit_level) const {
    if (explicit_level) return *explicit_level;
    if (!agent_ || !agent_->state().model.reasoning) {
        return services_.settings_manager &&
                services_.settings_manager->settings().default_thinking_level
            ? *services_.settings_manager->settings().default_thinking_level
            : "medium";
    }
    return agent_->state().thinking_level;
}

/// pi `_cycleScopedModel`/`_cycleAvailableModel` shared tail: apply the model
/// (pi `agent.state.model = model`), append the `model_change` entry, write
/// the global settings default, and re-clamp the thinking level — the same
/// persistence sequence as `set_model`.
[[nodiscard]] boost::asio::awaitable<util::ExpectedVoid>
AgentSessionRuntime::apply_model_switch(
    ai::Model model,
    std::string thinking_level) {
    if (auto swapped = agent_->set_model(std::move(model)); !swapped) {
        co_return std::unexpected(std::move(swapped.error()));
    }
    const auto& active_model = agent_->state().model;

    // Persist the `model_change` session entry (pi `appendModelChange`).
    // In-memory sessions have no v3 tree entry surface and are not resumable,
    // so the entry is skipped there exactly like the creation-time entry.
    if (auto* jsonl_store =
            dynamic_cast<harness::session::JsonlSessionStore*>(session_.store.get())) {
        if (auto appended = jsonl_store->append_model_change(
                std::nullopt, active_model.provider, active_model.id);
            !appended) {
            co_return std::unexpected(std::move(appended.error()));
        }
    }

    // pi `settingsManager.setDefaultModelAndProvider` (global scope).
    if (services_.settings_manager) {
        if (auto saved = services_.settings_manager->set_default_model_and_provider(
                active_model.provider, active_model.id);
            !saved) {
            co_return std::unexpected(std::move(saved.error()));
        }
    }

    // Re-clamp the thinking level for the new model's capabilities (pi
    // `setThinkingLevel` after the model assignment; entry + settings writes
    // ride the existing thinking-level path).
    if (auto clamped = set_thinking_level(std::move(thinking_level)); !clamped) {
        co_return std::unexpected(std::move(clamped.error()));
    }
    // The model Bash Tool reads the live model at execution time.
    refresh_bash_session_environment();
    co_return util::ExpectedVoid{};
}

boost::asio::awaitable<util::Expected<std::optional<ModelCycleResult>>>
AgentSessionRuntime::cycle_model(std::string_view direction) {
    if (auto rejected = reject_if_closed(); !rejected) {
        co_return std::unexpected(rejected.error());
    }
    if (!agent_ || !services_.model_runtime) {
        co_return std::unexpected(util::make_error(
            util::ErrorCode::Validation, "session is closed"));
    }
    const bool forward = direction != "backward";

    // Scoped path (pi `_cycleScopedModel`): filter the scoped set by
    // configured auth, then cycle within it.
    if (!scoped_models_.empty()) {
        std::vector<cch::coding_agent::ScopedModel> eligible;
        for (const auto& scoped : scoped_models_) {
            auto checked = co_await services_.model_runtime->check_auth(scoped.model.provider);
            if (!checked) {
                co_return std::unexpected(std::move(checked.error()));
            }
            if (*checked) eligible.push_back(scoped);
        }
        if (eligible.size() <= 1) {
            co_return std::optional<ModelCycleResult>{};
        }

        const auto& current = agent_->state().model;
        std::size_t current_index = 0;
        for (std::size_t index = 0; index < eligible.size(); ++index) {
            if (eligible[index].model.provider == current.provider &&
                eligible[index].model.id == current.id) {
                current_index = index;
                break;
            }
        }
        const auto next_index = forward
            ? (current_index + 1) % eligible.size()
            : (current_index + eligible.size() - 1) % eligible.size();
        const auto& next = eligible[next_index];
        const auto thinking_level =
            resolve_thinking_level_for_switch(next.thinking_level);
        if (auto applied = co_await apply_model_switch(next.model, thinking_level);
            !applied) {
            co_return std::unexpected(std::move(applied.error()));
        }
        co_return ModelCycleResult{
            .model = agent_->state().model,
            .thinking_level = agent_->state().thinking_level,
            .is_scoped = true,
        };
    }

    // Available path (pi `_cycleAvailableModel`): cycle within the
    // auth-filtered availability snapshot.
    auto available = co_await services_.model_runtime->get_available();
    if (!available) {
        co_return std::unexpected(std::move(available.error()));
    }
    if (available->size() <= 1) {
        co_return std::optional<ModelCycleResult>{};
    }

    const auto& current = agent_->state().model;
    std::size_t current_index = 0;
    for (std::size_t index = 0; index < available->size(); ++index) {
        if ((*available)[index].provider == current.provider &&
            (*available)[index].id == current.id) {
            current_index = index;
            break;
        }
    }
    const auto next_index = forward
        ? (current_index + 1) % available->size()
        : (current_index + available->size() - 1) % available->size();
    const auto& next_model = (*available)[next_index];
    const auto thinking_level = resolve_thinking_level_for_switch(std::nullopt);
    if (auto applied = co_await apply_model_switch(next_model, thinking_level);
        !applied) {
        co_return std::unexpected(std::move(applied.error()));
    }
    co_return ModelCycleResult{
        .model = agent_->state().model,
        .thinking_level = agent_->state().thinking_level,
        .is_scoped = false,
    };
}

util::Expected<std::optional<std::string>>
AgentSessionRuntime::cycle_thinking_level() {
    if (auto rejected = reject_if_closed(); !rejected) {
        return std::unexpected(rejected.error());
    }
    if (!agent_) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation, "session is closed"));
    }
    // pi `supportsThinking()`: the active model must support reasoning.
    if (!agent_->state().model.reasoning) {
        return std::optional<std::string>{};
    }
    const auto levels = ai::get_supported_thinking_levels(agent_->state().model);
    if (levels.empty()) {
        return std::optional<std::string>{};
    }
    const auto current = agent_->state().thinking_level;
    // pi `levels.indexOf(current)`: a level absent from the supported set
    // yields -1, so the next index is 0 (the first supported level).
    std::ptrdiff_t current_index = -1;
    for (std::size_t index = 0; index < levels.size(); ++index) {
        if (ai::detail::model_thinking_level_name(levels[index]) == current) {
            current_index = static_cast<std::ptrdiff_t>(index);
            break;
        }
    }
    const auto next_index =
        (current_index + 1) % static_cast<std::ptrdiff_t>(levels.size());
    const auto next_name = ai::detail::model_thinking_level_name(levels[next_index]);
    if (!next_name) {
        return std::optional<std::string>{};
    }
    auto applied = set_thinking_level(*next_name);
    if (!applied) {
        return std::unexpected(std::move(applied.error()));
    }
    return std::optional<std::string>{*applied};
}

void AgentSessionRuntime::set_scoped_models(
    std::vector<cch::coding_agent::ScopedModel> models) {
    scoped_models_ = std::move(models);
}

// ── Tree navigation (pi navigateTree, G2 decision 13) ──────────────────────

namespace {

/// The user message text for the tree editor pre-fill (pi `contentText`).
[[nodiscard]] std::string user_message_text(const ai::UserMessage& message) {
    if (const auto* text = std::get_if<std::string>(&message.content)) {
        return *text;
    }
    return ai::text_from_user_message(message);
}

/// The custom-message text for the tree editor pre-fill (pi `contentText` on
/// the custom-message content).
[[nodiscard]] std::string custom_message_text(
    const harness::session::CustomMessageEntryValue& value) {
    if (const auto* text = std::get_if<std::string>(&value.content)) {
        return *text;
    }
    std::string result;
    for (const auto& block :
         std::get<std::vector<harness::session::CustomMessageEntryContentBlock>>(
             value.content)) {
        if (const auto* text = std::get_if<ai::TextContent>(&block)) {
            result += text->text;
        }
    }
    return result;
}

/// pi `navigateTree` leaf semantics: the new leaf position and editor text
/// for one target entry. A user or custom message moves the leaf to its
/// effective parent (the explicit wire parent, or the inferred linear-chain
/// parent for the C++ flat-file shape; nullopt at the root) and returns the
/// message text; any other target becomes the leaf with no editor text.
struct TreeLeafDecision {
    std::optional<std::string> new_leaf_id;
    std::optional<std::string> editor_text;
};

[[nodiscard]] TreeLeafDecision tree_leaf_decision(
    std::optional<std::string> effective_parent,
    const harness::session::SessionEntry& entry);

[[nodiscard]] TreeLeafDecision tree_leaf_decision(
    const harness::session::SessionTree& tree,
    const harness::session::SessionEntry& entry) {
    return tree_leaf_decision(tree.effective_parent_id(entry.entry_id), entry);
}

[[nodiscard]] TreeLeafDecision tree_leaf_decision(
    std::optional<std::string> effective_parent,
    const harness::session::SessionEntry& entry) {
    if (entry.kind == harness::session::SessionEntryKind::Message &&
        entry.message.has_value()) {
        if (const auto* user = std::get_if<ai::UserMessage>(&*entry.message)) {
            auto text = user_message_text(*user);
            return TreeLeafDecision{
                .new_leaf_id = std::move(effective_parent),
                .editor_text = std::move(text),
            };
        }
    } else if (entry.kind == harness::session::SessionEntryKind::CustomMessage) {
        if (const auto* value =
                std::get_if<harness::session::CustomMessageEntryValue>(
                    &entry.value)) {
            auto text = custom_message_text(*value);
            return TreeLeafDecision{
                .new_leaf_id = std::move(effective_parent),
                .editor_text = std::move(text),
            };
        }
    }
    return TreeLeafDecision{.new_leaf_id = entry.entry_id, .editor_text = std::nullopt};
}

/// pi `setLeafId(null)` root leaf marker: `targetId: null` on the wire.
[[nodiscard]] util::ExpectedVoid persist_leaf_marker(
    harness::session::JsonlSessionStore& store,
    const std::optional<std::string>& new_leaf_id) {
    return store.append_leaf(std::nullopt, new_leaf_id);
}

/// In-memory entries derived from the live context (the C++ in-memory store
/// keeps no entries, so the tree surface synthesizes ids like the fork
/// flow's `mem-<index>` ids, which never surface outside the tree surface).
/// Each entry remembers its live-message index so in-memory navigation can
/// truncate the live context exactly at the chosen point.
struct LiveContextEntry {
    harness::session::SessionEntry entry;
    std::size_t message_index{0};
};

[[nodiscard]] std::vector<LiveContextEntry>
live_context_entries(const std::vector<ai::MessageVariant>& messages) {
    std::vector<LiveContextEntry> result;
    std::optional<std::string> previous_id;
    std::size_t entry_index = 0;
    for (std::size_t message_index = 0; message_index < messages.size();
         ++message_index) {
        const auto& message = messages[message_index];
        // The live context never carries SystemMessages (the session system
        // prompt is delivered per run through `AgentContext.system_prompt`);
        // skip defensively so the id-to-message mapping stays exact.
        if (std::holds_alternative<ai::SystemMessage>(message)) {
            continue;
        }
        harness::session::SessionEntry entry;
        entry.kind = harness::session::SessionEntryKind::Message;
        entry.message = message;
        entry.entry_id = "mem-" + std::to_string(entry_index);
        entry.parent_id = previous_id;
        if (const auto* user = std::get_if<ai::UserMessage>(&message)) {
            entry.timestamp = user->timestamp;
        } else if (const auto* assistant =
                       std::get_if<ai::AssistantMessage>(&message)) {
            entry.timestamp = assistant->timestamp;
        } else if (const auto* tool =
                       std::get_if<ai::ToolResultMessage>(&message)) {
            entry.timestamp = tool->timestamp;
        } else if (const auto* bash =
                       std::get_if<ai::BashExecutionMessage>(&message)) {
            entry.timestamp = bash->timestamp;
        } else if (const auto* custom =
                       std::get_if<ai::CustomMessage>(&message)) {
            entry.timestamp = custom->timestamp;
            entry.kind = harness::session::SessionEntryKind::CustomMessage;
            harness::session::CustomMessageEntryValue value;
            value.custom_type = custom->custom_type;
            std::vector<harness::session::CustomMessageEntryContentBlock> blocks;
            for (const auto& content : custom->content) {
                // The custom-message wire content carries text/image blocks
                // only (pi CustomMessageEntryContent); thinking blocks do
                // not project onto it.
                if (std::holds_alternative<ai::TextContent>(content)) {
                    blocks.push_back(std::get<ai::TextContent>(content));
                } else if (std::holds_alternative<ai::ImageContent>(content)) {
                    blocks.push_back(std::get<ai::ImageContent>(content));
                }
            }
            value.content = std::move(blocks);
            value.display = custom->display;
            value.details = custom->details;
            entry.value = std::move(value);
        } else if (const auto* summary =
                       std::get_if<ai::BranchSummaryMessage>(&message)) {
            entry.timestamp = summary->timestamp;
            entry.kind = harness::session::SessionEntryKind::BranchSummary;
            harness::session::BranchSummaryEntryValue value;
            value.from_id = summary->from_id;
            value.summary = summary->summary;
            entry.value = std::move(value);
        } else if (const auto* compaction =
                       std::get_if<ai::CompactionSummaryMessage>(&message)) {
            entry.timestamp = compaction->timestamp;
            entry.kind = harness::session::SessionEntryKind::Compaction;
            harness::session::CompactionEntryValue value;
            value.summary = compaction->summary;
            value.tokens_before =
                static_cast<std::size_t>(compaction->tokens_before);
            entry.value = std::move(value);
        }
        previous_id = entry.entry_id;
        result.push_back(LiveContextEntry{
            .entry = std::move(entry),
            .message_index = message_index,
        });
        ++entry_index;
    }
    return result;
}

/// Build the tree roots for the in-memory surface: a linear chain with the
/// live messages' timestamps, no labels (the in-memory store keeps none).
[[nodiscard]] std::vector<harness::session::SessionTreeNode>
build_linear_tree(const std::vector<LiveContextEntry>& entries) {
    std::vector<harness::session::SessionTreeNode> nodes;
    nodes.reserve(entries.size());
    for (const auto& live : entries) {
        harness::session::SessionTreeNode node;
        node.entry = live.entry;
        nodes.push_back(std::move(node));
    }
    if (nodes.empty()) {
        return {};
    }
    for (std::size_t index = 0; index + 1 < nodes.size(); ++index) {
        nodes[index].children.push_back(std::move(nodes[index + 1]));
    }
    nodes.resize(1);
    return nodes;
}

} // namespace

boost::asio::awaitable<util::ExpectedVoid>
AgentSessionRuntime::wait_for_idle() {
    // pi `waitForIdle`: settle when an Agent run is active. The run in
    // flight continues to its normal terminal; the settled signal is
    // cancelled exactly when the run settles (same waiter-before-cancel
    // ordering as PendingUserBashCommit). The wait itself cannot fail.
    if (!prompt_active_ || !prompt_settled_signal_) {
        co_return util::ExpectedVoid{};
    }
    boost::system::error_code wait_error;
    co_await prompt_settled_signal_->async_wait(
        boost::asio::redirect_error(
            boost::asio::use_awaitable, wait_error));
    co_return util::ExpectedVoid{};
}

util::Expected<coding_agent::SessionTreeTopology>
AgentSessionRuntime::session_tree() const {
    if (auto rejected = reject_if_closed(); !rejected) {
        return std::unexpected(rejected.error());
    }
    auto* jsonl_store =
        dynamic_cast<harness::session::JsonlSessionStore*>(session_.store.get());
    if (jsonl_store != nullptr && jsonl_store->path()) {
        auto tree = harness::session::JsonlSessionStore::open_as_tree(
            *jsonl_store->path());
        if (!tree) {
            return std::unexpected(tree.error());
        }
        return coding_agent::SessionTreeTopology{
            .roots = tree->get_tree(),
            .leaf_id = tree->leaf_id(),
        };
    }
    // In-memory: derive a linear tree from the live context (synthetic ids).
    const auto entries =
        agent_ ? live_context_entries(agent_->state().messages)
               : std::vector<LiveContextEntry>{};
    std::string leaf_id;
    if (!entries.empty()) {
        leaf_id = entries.back().entry.entry_id;
    }
    return coding_agent::SessionTreeTopology{
        .roots = build_linear_tree(entries),
        .leaf_id = std::move(leaf_id),
    };
}

util::Expected<coding_agent::TreeNavigationResult>
AgentSessionRuntime::navigate_tree(std::string_view target_id) {
    if (auto rejected = reject_if_closed(); !rejected) {
        return std::unexpected(rejected.error());
    }
    // pi's navigateTree streaming guard: the interactive flow aborts the
    // active response (then waits for settle) before navigating; a direct
    // call while a run is active is rejected verbatim (regression
    // tree-during-streaming).
    if (prompt_active_) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation,
            "Wait for the current response to finish before navigating the session tree."));
    }
    if (!agent_) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Validation, "session is closed"));
    }

    auto* jsonl_store =
        dynamic_cast<harness::session::JsonlSessionStore*>(session_.store.get());
    if (jsonl_store != nullptr && jsonl_store->path()) {
        auto tree = harness::session::JsonlSessionStore::open_as_tree(
            *jsonl_store->path());
        if (!tree) {
            return std::unexpected(tree.error());
        }

        // pi: no-op when already at the target.
        if (target_id == tree->leaf_id()) {
            return coding_agent::TreeNavigationResult{};
        }
        const auto* target = tree->getEntry(target_id);
        if (target == nullptr) {
            return std::unexpected(util::make_error(
                util::ErrorCode::Session,
                std::format("Entry {} not found", target_id)));
        }

        const auto decision = tree_leaf_decision(*tree, *target);

        // Switch the leaf: branch (non-root) or reset (root position).
        if (decision.new_leaf_id.has_value()) {
            if (auto branched = tree->branch(*decision.new_leaf_id); !branched) {
                return std::unexpected(branched.error());
            }
        } else {
            tree->reset_leaf();
        }

        // Persist the leaf marker first; on failure nothing durable changed
        // and the live context keeps the old path (Session Event Commitment
        // ordering: the durable mutation precedes the live-state advance).
        if (auto persisted =
                persist_leaf_marker(*jsonl_store, decision.new_leaf_id);
            !persisted) {
            return std::unexpected(persisted.error());
        }

        // Rebuild the live Agent context from the new path (pi
        // `agent.state.messages = sessionContext.messages`).
        const auto context = tree->buildSessionContext();
        if (auto replaced =
                agent::detail::AgentMessageAccess::replace_messages(
                    *agent_, context.messages);
            !replaced) {
            return std::unexpected(replaced.error());
        }
        return coding_agent::TreeNavigationResult{
            .editor_text = decision.editor_text,
            .cancelled = false,
        };
    }

    // In-memory: navigate over the live context (synthetic ids, linear). The
    // leaf semantics mirror the persisted path: a user message moves the
    // leaf before it (its text returns to the editor); any other target
    // becomes the leaf. There is no store, so nothing persists.
    const auto entries =
        live_context_entries(agent_->state().messages);
    const LiveContextEntry* target = nullptr;
    for (const auto& live : entries) {
        if (live.entry.entry_id == target_id) {
            target = &live;
            break;
        }
    }
    if (target == nullptr) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Session,
            std::format("Entry {} not found", target_id)));
    }
    // pi: no-op when already at the target (the in-memory leaf is the last
    // live-context entry).
    if (target->entry.entry_id == entries.back().entry.entry_id) {
        return coding_agent::TreeNavigationResult{};
    }
    const auto decision = tree_leaf_decision(target->entry.parent_id, target->entry);
    auto messages = agent_->state().messages;
    std::optional<std::string> editor_text = decision.editor_text;
    if (decision.new_leaf_id.has_value()) {
        // The new leaf is the target's parent: keep live messages through
        // the new leaf, dropping the target message and everything after it
        // (pi: the live context becomes the path to the new leaf).
        std::optional<std::size_t> keep_through_message;
        for (const auto& live : entries) {
            if (live.entry.entry_id == *decision.new_leaf_id) {
                keep_through_message = live.message_index;
                break;
            }
        }
        if (keep_through_message &&
            *keep_through_message + 1 <= messages.size()) {
            messages.resize(*keep_through_message + 1);
        }
    } else {
        // Root position: live context ends before the first entry (pi
        // `resetLeaf`).
        messages.clear();
    }
    if (auto replaced =
            agent::detail::AgentMessageAccess::replace_messages(
                *agent_, std::move(messages));
        !replaced) {
        return std::unexpected(replaced.error());
    }
    return coding_agent::TreeNavigationResult{
        .editor_text = std::move(editor_text),
        .cancelled = false,
    };
}

util::ExpectedVoid AgentSessionRuntime::set_entry_label(
    std::string_view entry_id,
    std::optional<std::string> label) {
    if (auto rejected = reject_if_closed(); !rejected) {
        return std::unexpected(rejected.error());
    }
    auto* jsonl_store =
        dynamic_cast<harness::session::JsonlSessionStore*>(session_.store.get());
    if (jsonl_store == nullptr || !jsonl_store->path()) {
        // In-memory sessions keep no entry surface; the change is dropped
        // like every in-memory store write (the tree's own display keeps it
        // for the session's lifetime).
        return {};
    }
    auto tree = harness::session::JsonlSessionStore::open_as_tree(
        *jsonl_store->path());
    if (!tree) {
        return std::unexpected(tree.error());
    }
    if (tree->getEntry(entry_id) == nullptr) {
        return std::unexpected(util::make_error(
            util::ErrorCode::Session,
            std::format("Entry {} not found", entry_id)));
    }
    // pi `appendLabelChange`: the label entry hangs under the current leaf
    // (null at the root position).
    std::optional<std::string> parent_id;
    if (!tree->leaf_id().empty()) {
        parent_id = tree->leaf_id();
    }
    return jsonl_store->append_label_change(
        std::move(parent_id), std::string{entry_id}, std::move(label));
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
                boost::asio::redirect_error(
                    boost::asio::use_awaitable, wait_error));
        }
    }

    util::Expected<coding_agent::CompactionResult> result;
    try {
        result = co_await compact_impl(std::move(custom_instructions));
    } catch (...) {
        const auto failure = prompt_exception(std::current_exception());
        result = std::unexpected(failure.error());
    }
    compaction_active_ = false;
    emit_session_event(CompactionEndEvent{
        .reason = "manual",
        .aborted = false,
        .error_message = result
            ? std::nullopt
            : std::optional<std::string>{result.error().message},
    });
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
        [runtime = auth_guided_runtime_, model](
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
    bool will_retry,
    std::string reason) {
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

    // pi `_runAutoCompaction`: the auto trigger emits `compaction_start`
    // with its reason before summarizing.
    emit_session_event(CompactionStartEvent{.reason = reason});
    auto result = co_await execute_compaction(prep, settings, {});
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
        if (auto popped =
                agent::detail::AgentMessageAccess::pop_trailing_assistant(*agent_);
            !popped) {
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
    // message. Overflow never routes to turn auto-retry: the post-run loop
    // excludes it via `is_retryable_error` before reaching this branch, so
    // the two recovery paths never interfere (T12's boundary).
    if (same_model && harness::session::is_context_overflow(
                          assistant_message, context_window)) {
        const bool will_retry =
            assistant_message.stop_reason != ai::AssistantStopReason::Stop;
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
        if (auto popped =
                agent::detail::AgentMessageAccess::pop_trailing_assistant(*agent_);
            !popped) {
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
        if (co_await run_auto_compaction(false, "threshold")) {
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

// ── Turn auto-retry (T12) ───────────────────────────────────────────────────

RetrySettings AgentSessionRuntime::effective_retry_settings() const {
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
        settings.max_retries =
            static_cast<std::size_t>(*configured->max_retries);
    }
    if (configured->base_delay_ms) {
        settings.base_delay_ms =
            static_cast<std::size_t>(*configured->base_delay_ms);
    }
    return settings;
}

bool AgentSessionRuntime::is_retryable_error(
    const ai::AssistantMessage& message) const {
    // Context overflow is handled by compaction, never by retry (pi
    // `_isRetryableError`); the two recovery paths never interfere (T10's
    // boundary).
    const auto& model = agent_->state().model;
    const std::size_t context_window =
        static_cast<std::size_t>(model.context_window);
    if (harness::session::is_context_overflow(message, context_window)) {
        return false;
    }
    return ai::is_retryable_assistant_error(message);
}

boost::asio::awaitable<bool> AgentSessionRuntime::prepare_retry(
    const ai::AssistantMessage& message,
    std::stop_token stop_token) {
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

    const auto delay_ms =
        settings.base_delay_ms *
        (static_cast<std::size_t>(1) << (retry_attempt_ - 1));
    emit_session_event(AutoRetryStartEvent{
        .attempt = retry_attempt_,
        .max_attempts = static_cast<int>(settings.max_retries),
        .delay_ms = static_cast<std::int64_t>(delay_ms),
        .error_message =
            message.error_message.value_or("Unknown error"),
    });

    // Remove the failed assistant message from live state; it stays in
    // session history (pi `_prepareRetry` `messages.slice(0, -1)`), so the
    // continuation's last message is a user or tool-result message.
    if (auto popped =
            agent::detail::AgentMessageAccess::pop_trailing_assistant(*agent_);
        !popped) {
        co_return false;
    }

    // Abort-interruptible exponential backoff sleep (pi `sleep(delayMs,
    // this._retryAbortController.signal)`): a prompt-scoped abort cancels the
    // timer, and an already-requested stop aborts the wait immediately.
    boost::asio::steady_timer timer(
        co_await boost::asio::this_coro::executor);
    timer.expires_after(std::chrono::milliseconds(delay_ms));
    boost::system::error_code wait_error;
    std::stop_callback cancel_wait{
        stop_token, [&timer]() { timer.cancel(); }};
    co_await timer.async_wait(
        boost::asio::redirect_error(boost::asio::use_awaitable, wait_error));
    if (wait_error == boost::asio::error::operation_aborted ||
        stop_token.stop_requested()) {
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
    return skills_;
}

const std::vector<PromptTemplate>& AgentSessionRuntime::templates() const {
    return templates_;
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
    // Drop the session's request-time decorator handle; the Agent released its
    // reference above, so the canonical runtime is freed once the owned
    // reference below is reset (runtimes carry no dispose ceremony).
    auth_guided_runtime_.reset();
    skills_.clear();
    templates_.clear();
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

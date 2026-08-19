#include "AgentSessionRuntime.hpp"

#include <cch/ai/Content.hpp>
#include <cch/coding_agent/AuthGuidance.hpp>
#include <cch/coding_agent/AgentConfigDir.hpp>
#include <cch/coding_agent/Settings.hpp>
#include <cch/agent/harness/session/SessionStore.hpp>
#include <cch/agent/harness/session/SessionTree.hpp>
#include "agent/harness/WorkspaceFileSystem.hpp"

#include "agent/AgentMessageAccess.hpp"
#include "agent/AgentPromptAccess.hpp"
#include "ai/AsyncResultBridge.hpp"
#include "ai/ModelThinkingLevel.hpp"
#include "ai/utils/RetryClassifier.hpp"
#include "coding_agent/BoundedText.hpp"
#include "coding_agent/ProjectResourceLoader.hpp"
#include <cch/coding_agent/ProjectTrust.hpp>
#include "coding_agent/SkillFormatting.hpp"
#include "coding_agent/prompt/PromptExpansion.hpp"
#include "coding_agent/prompt/PromptTemplateExpander.hpp"
#include "coding_agent/prompt/SystemPromptBuilder.hpp"
#include "coding_agent/runtime/AuthGuidanceStream.hpp"
#include "coding_agent/runtime/UserBashOutputAccumulator.hpp"
#include "agent/harness/compaction/Compaction.hpp"
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

/// Bounded, redacted session-event observer diagnostic (ADR 0017): the
/// session-assembly mirror of the Agent's weak-observer diagnostics channel.
constexpr std::size_t kMaxSessionObserverDiagnostics = 16;
constexpr std::size_t kMaxSessionObserverDetailBytes = 1024;

void record_session_observer_diagnostic(
    std::vector<support::Error>& diagnostics,
    const support::Error& failure) {
    std::string detail = failure.message;
    if (!failure.detail.empty()) {
        detail += ": ";
        detail += failure.detail;
    }
    detail = ai::bounded_redacted_text(
        std::move(detail), kMaxSessionObserverDetailBytes, "...");
    if (diagnostics.size() == kMaxSessionObserverDiagnostics) {
        diagnostics.erase(diagnostics.begin());
    }
    diagnostics.push_back(support::make_error(
        failure.code,
        "session event observer failed",
        std::move(detail)));
}

} // namespace

ai::ModelStreamFactory AgentSessionRuntime::make_stream_factory() {
    auto models = services_.model_runtime->ai_models();
    auto runtime = services_.model_runtime;
    return ai::ModelStreamFactory{
        [models, runtime](
            ai::Model model,
            ai::AiContext context,
            ai::SimpleStreamOptions options)
            -> ai::ModelStream {
            // The provider identity must survive the move into the Models
            // call; the forwarding sink may fire after the model argument is
            // already moved-from.
            const std::string provider{model.provider};
            auto inner = models->stream(
                std::move(model), std::move(context), std::move(options));
            return apply_auth_guidance(
                std::move(inner),
                provider,
                OAuthProviderPredicate{[runtime](std::string_view provider_id) {
                    return runtime->is_using_oauth(provider_id);
                }},
                std::filesystem::path{kDefaultAuthGuidanceDocsPath});
        }};
}

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
    // The Session Event Commitment channel exists only for persistent
    // sessions with a Runtime mailbox; in-memory sessions need no channel
    // and their sink stays a successful no-op (ADR 0040).
    if (session_.store && session_.store->path() && services_.runtime_target) {
        persistence_ = std::make_shared<SessionPersistence>(
            session_.store, services_.runtime_target);
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
    options.convert_to_llm = [](
                                 std::vector<ai::MessageVariant> messages) {
        std::erase_if(messages, [](const ai::MessageVariant& message) {
            const auto* bash = std::get_if<ai::BashExecutionMessage>(&message);
            return bash != nullptr && bash->exclude_from_context;
        });
        return support::AsyncResult<std::vector<ai::MessageVariant>>{
            std::move(messages)};
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
                prompt_tool_guidelines_.end(),
                metadata->guidelines.begin(),
                metadata->guidelines.end());
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
        session_.context_thinking_level.value_or(
            config_.default_thinking_level.value_or("medium"));

    // Request-time re-auth guidance (pi `_getRequiredRequestAuth`): the
    // Agent's stream and the summarization seam run through a session-layer
    // ModelStream decorator that rewrites auth/oauth-category terminal
    // failures to pi's two verbatim guidance branches.

    // Construct Agent last: it holds the AI-owned ModelStream factory (ADR
    // 0040 / #453) and takes sole ownership of the move-only tool registry.
    agent_.emplace(
        make_stream_factory(),
        std::move(services_.tools),
        std::move(options),
        std::move(initial_state));

    // Expose the live session facts to the model Bash Tool (pi
    // `resolveSpawnContext`); the Agent's clamped state is authoritative.
    refresh_bash_session_environment();
}

std::string AgentSessionRuntime::rebuild_system_prompt() const {
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
        for (std::size_t index = 1; index < config_.append_system_prompt.size();
             ++index) {
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
        prompt_options.promptGuidelines.end(),
        prompt_tool_guidelines_.begin(),
        prompt_tool_guidelines_.end());
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
    return buildSystemPrompt(prompt_options);
}

boost::asio::awaitable<support::Expected<AgentSessionReloadResult>>
AgentSessionRuntime::reload() {
    if (auto rejected = reject_if_closed(); !rejected) {
        co_return std::unexpected(rejected.error());
    }
    // pi `AgentSession.reload()`: `settingsManager.reload()` first
    // (preserving `projectTrusted`), then `resourceLoader.reload()` — the
    // retained discovery request re-run with the creation-time trust state.
    if (services_.settings_manager) {
        (void)services_.settings_manager->reload();
    }
    if (!config_.resource_loading_request) {
        co_return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "session has no retained resource loading request"));
    }
    auto fs = harness::WorkspaceFileSystem::create(session_.workspace);
    if (!fs) {
        co_return std::unexpected(support::make_error(
            support::ErrorCode::Unknown,
            "reload failed: could not open the session workspace",
            fs.error().message));
    }
    auto resource_request = *config_.resource_loading_request;
    // pi `reload()` preserves `SettingsManager.projectTrusted`: the reload
    // re-runs with the current trust state, never re-resolving it.
    resource_request.project_trust_override = is_project_trusted();
    resource_request.workspace = session_.workspace;
    ProjectTrustStore trust_store{coding_agent::trust_store_file_path()};
    auto loading = load_project_resources(*fs, trust_store, std::move(resource_request));
    if (!loading.fatal_errors.empty()) {
        co_return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "reload failed",
            loading.fatal_errors.front().message));
    }

    // Swap the live resource snapshots and the System Prompt inputs (pi
    // `_rebuildSystemPrompt` reads the fresh loader results).
    skills_ = std::move(loading.resources.skills);
    templates_ = std::move(loading.resources.prompt_templates);
    config_.custom_prompt = std::move(loading.resources.system_prompt);
    config_.append_system_prompt = std::move(loading.resources.append_system_prompt);
    config_.context_files = std::move(loading.resources.agents_files);
    config_.system_prompt_source =
        std::move(loading.resources.system_prompt_source);
    config_.append_system_prompt_sources =
        std::move(loading.resources.append_system_prompt_sources);
    config_.skill_diagnostics = std::move(loading.skill_diagnostics);
    config_.prompt_diagnostics = std::move(loading.prompt_diagnostics);
    config_.theme_diagnostics = std::move(loading.theme_diagnostics);

    // Rebuild the System Prompt and push it into the live Agent (pi
    // `_rebuildSystemPrompt` → `agent.state.systemPrompt`).
    if (!agent_) {
        co_return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "session Agent is unavailable"));
    }
    agent_->set_system_prompt(rebuild_system_prompt());

    AgentSessionReloadResult result;
    result.skill_diagnostics = config_.skill_diagnostics;
    result.prompt_diagnostics = config_.prompt_diagnostics;
    result.theme_diagnostics = config_.theme_diagnostics;
    result.themes = std::move(loading.resources.themes);
    co_return result;
}

support::ExpectedVoid AgentSessionRuntime::reject_if_closed() const {
    if (lifecycle_ != Lifecycle::Open) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
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

support::ExpectedVoid AgentSessionRuntime::reject_if_busy() const {
    if (prompt_active_) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "session is busy (prompt already in flight)"));
    }
    if (compaction_active_) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "session is busy (compaction already in flight)"));
    }
    return {};
}

support::ExpectedVoid AgentSessionRuntime::reject_if_user_bash_busy() const {
    if (user_bash_active_) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "a User Bash command is already in flight"));
    }
    return {};
}

support::ExpectedVoid AgentSessionRuntime::commit_user_bash_completion(
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
            entry->commit_result = std::unexpected(support::make_error(
                support::ErrorCode::Validation,
                "session Agent is unavailable"));
        }
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
        try {
#endif
            (void)entry->committed_signal.cancel();
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
        } catch (...) {
            // Releasing the awaiting coroutine is best-effort; the commitment
            // above is the authoritative outcome.
        }
#endif
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

boost::asio::awaitable<support::ExpectedVoid>
AgentSessionRuntime::preflight_auth_guidance() {
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
    auto checked = co_await services_.model_runtime->check_auth(model.provider);
    if (!checked) {
        co_return std::unexpected(std::move(checked.error()));
    }
    if (*checked) {
        co_return support::ExpectedVoid{};
    }
    const std::string provider{model.provider};
    if (services_.model_runtime->is_using_oauth(provider)) {
        co_return std::unexpected(support::make_error(
            support::ErrorCode::Auth,
            format_oauth_reauthenticate_message(provider)));
    }
    co_return std::unexpected(support::make_error(
        support::ErrorCode::Auth,
        format_no_api_key_found_message(
            provider,
            std::filesystem::path{kDefaultAuthGuidanceDocsPath})));
}

boost::asio::awaitable<support::ExpectedVoid> AgentSessionRuntime::run_prompt(
    std::string prompt,
    std::vector<ai::ImageContent> images,
    bool expand_prompt_templates,
    std::move_only_function<support::ExpectedVoid()> on_preflight_accepted) {
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
            co_return std::unexpected(support::make_error(
                support::ErrorCode::Session,
                "session persistence failed; rejecting new prompt",
                failure->detail.empty() ? failure->message : failure->detail));
        }
    } else if (session_.store && session_.store->path()) {
        co_return std::unexpected(support::make_error(
            support::ErrorCode::Session,
            "session persistence is unavailable; rejecting new prompt"));
    }

    prompt_active_ = true;
    active_stop_source_.emplace();
    // A concurrent manual compaction awaits this signal after requesting run
    // cancellation; it is cancelled exactly when the run settles (the same
    // waiter-before-cancel ordering as PendingUserBashCommit).
    prompt_settled_signal_.emplace(co_await boost::asio::this_coro::executor);
    prompt_settled_signal_->expires_at(
        std::chrono::steady_clock::time_point::max());

    support::ExpectedVoid result;
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
    try {
#endif
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
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
    } catch (const std::exception& error) {
        result = std::unexpected(support::make_error(
            support::ErrorCode::Unknown,
            "session prompt coroutine failed",
            error.what()));
    } catch (...) {
        result = std::unexpected(support::make_error(
            support::ErrorCode::Unknown,
            "session prompt coroutine failed"));
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
        if (!user_bash_active_ && !compaction_active_) {
            co_await finalize_close_after_active_work();
        }
    }
    co_return result;
}

boost::asio::awaitable<support::Expected<UserBashCompletion>>
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
        co_return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "User Shell is unavailable"));
    }

    user_bash_active_ = true;
    active_user_bash_stop_source_.emplace();
    const auto recorded_command = command;
    support::Expected<UserShellResult> shell_result = std::unexpected(support::make_error(
        support::ErrorCode::Unknown,
        "User Shell execution did not finish"));
    if (progress_sink) {
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
        try {
#endif
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
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
        } catch (const std::exception& error) {
            active_user_bash_stop_source_.reset();
            user_bash_active_ = false;
            co_return std::unexpected(support::make_error(
                support::ErrorCode::Unknown,
                "User Bash progress callback failed",
                bounded_redacted_presentation(error.what())));
        } catch (...) {
            active_user_bash_stop_source_.reset();
            user_bash_active_ = false;
            co_return std::unexpected(support::make_error(
                support::ErrorCode::Unknown,
                "User Bash progress callback failed"));
        }
#endif
    }
    UserBashOutputAccumulator output;
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
    try {
#endif
        shell_result = co_await ai::detail::await_async_result(
            services_.user_shell->execute(
                std::move(command),
            [recorded_command, exclude_from_context, &output, &progress_sink](
                std::string_view update) -> support::ExpectedVoid {
                output.append(update);
                if (!progress_sink) return {};
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
                try {
#endif
                    return progress_sink(UserBashProgress{
                        .command = recorded_command,
                        .output = output.tail(),
                        .exclude_from_context = exclude_from_context,
                    });
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
                } catch (const std::exception& error) {
                    return std::unexpected(support::make_error(
                        support::ErrorCode::Unknown,
                        "User Bash progress callback failed",
                        error.what()));
                } catch (...) {
                    return std::unexpected(support::make_error(
                        support::ErrorCode::Unknown,
                        "User Bash progress callback failed"));
                }
#endif
            },
                active_user_bash_stop_source_->get_token()));
        if (shell_result) {
            output.finish();
        }
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
    } catch (const std::exception& error) {
        shell_result = std::unexpected(support::make_error(
            support::ErrorCode::Unknown,
            "User Shell execution failed",
            error.what()));
    } catch (...) {
        shell_result = std::unexpected(support::make_error(
            support::ErrorCode::Unknown,
            "User Shell execution failed",
            "unknown exception"));
    }
#endif

    active_user_bash_stop_source_.reset();
    user_bash_active_ = false;
    const auto finalize_if_last_active_work =
        [this]() -> boost::asio::awaitable<void> {
        if (lifecycle_ == Lifecycle::Closing && !prompt_active_ &&
            !compaction_active_) {
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
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
            try {
#endif
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
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
            } catch (...) {
                // Execution already completed; a presentation failure must not
                // lose the pending commitment.
            }
#endif
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
        co_return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
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

boost::asio::awaitable<support::ExpectedVoid> AgentSessionRuntime::run_agent_loop(
    ai::UserMessage prompt,
    std::stop_source stop_source) {
    if (!agent_) {
        co_return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "session Agent is unavailable"));
    }

    SessionEventCommitment commitment{persistence_};
    // The commitment sink also observes assistant message endings so the turn
    // auto-retry success event fires at the first non-error assistant message
    // (pi `_handleAgentEvent` message_end handler resets `_retryAttempt` and
    // emits `auto_retry_end success`). Rebuilt per call because the wrapped
    // sink is move-only and each prompt/continue takes it by value.
    const auto make_retry_observing_sink = [&]() {
        return agent::AgentEventCommitter{
            [this, inner = commitment.sink()](
                const agent::AgentLifecycleEvent& event) mutable
                -> support::ExpectedVoid {
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

    std::optional<support::ExpectedVoid> result;
    result = co_await ai::detail::await_async_result(
        agent::detail::AgentPromptAccess::prompt(
            *agent_,
            std::move(prompt),
            make_retry_observing_sink(),
            stop_source));
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
        const auto last_assistant =
            last_assistant_message_from(agent_->state().messages);
        if (!last_assistant) {
            break;
        }

        if (is_retryable_error(*last_assistant)) {
            if (co_await prepare_retry(
                    *last_assistant, stop_source.get_token())) {
                result = co_await ai::detail::await_async_result(
                    agent::detail::AgentPromptAccess::continue_run(
                        *agent_, make_retry_observing_sink(), stop_source));
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
            co_return co_await commitment.conclude(std::optional<support::ExpectedVoid>{
                std::unexpected(support::make_error(
                    support::ErrorCode::Stream,
                    std::string{kOverflowRecoveryFailedMessage}))});
        }
        if (outcome != AutoCompactionOutcome::OverflowRetry) {
            break;
        }
        result = co_await ai::detail::await_async_result(
            agent::detail::AgentPromptAccess::continue_run(
                *agent_, make_retry_observing_sink(), stop_source));
        if (!result) {
            break;
        }
    }
    co_return co_await commitment.conclude(std::move(result));
}

support::Expected<agent::AgentEventSubscription> AgentSessionRuntime::subscribe(
    agent::AgentEventSink sink) {
    if (auto rejected = reject_if_closed(); !rejected) {
        return std::unexpected(rejected.error());
    }
    if (!agent_) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
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

support::Expected<SessionEventSubscription>
AgentSessionRuntime::subscribe_session(AgentSessionEventSink sink) {
    if (auto rejected = reject_if_closed(); !rejected) {
        return std::unexpected(rejected.error());
    }
    if (!sink) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
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
                record_session_observer_diagnostic(
                    session_event_diagnostics_, observed.error());
                subscriber->registered = false;
                subscriber->delivery_enabled = false;
            }
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
        } catch (const std::exception& exception) {
            record_session_observer_diagnostic(
                session_event_diagnostics_,
                support::make_error(support::ErrorCode::Unknown, exception.what()));
            subscriber->registered = false;
            subscriber->delivery_enabled = false;
        } catch (...) {
            record_session_observer_diagnostic(
                session_event_diagnostics_,
                support::make_error(support::ErrorCode::Unknown, "unknown exception"));
            subscriber->registered = false;
            subscriber->delivery_enabled = false;
        }
#endif
    }
    std::erase_if(
        session_event_observers_,
        [](const std::shared_ptr<SessionSubscriber>& subscriber) {
            return !subscriber->registered;
        });
}

support::ExpectedVoid AgentSessionRuntime::steer(
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

support::ExpectedVoid AgentSessionRuntime::follow_up(
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

support::ExpectedVoid AgentSessionRuntime::set_steering_mode(agent::InputQueueMode mode) {
    if (auto rejected = reject_if_closed(); !rejected) {
        return rejected;
    }
    return agent_ ? agent_->set_steering_mode(mode) : std::unexpected(support::make_error(
        support::ErrorCode::Validation, "session is closed"));
}

support::ExpectedVoid AgentSessionRuntime::set_follow_up_mode(agent::InputQueueMode mode) {
    if (auto rejected = reject_if_closed(); !rejected) {
        return rejected;
    }
    return agent_ ? agent_->set_follow_up_mode(mode) : std::unexpected(support::make_error(
        support::ErrorCode::Validation, "session is closed"));
}

support::ExpectedVoid AgentSessionRuntime::clear_steering_queue() {
    if (auto rejected = reject_if_closed(); !rejected) {
        return rejected;
    }
    return agent_ ? agent_->clear_steering_queue() : std::unexpected(support::make_error(
        support::ErrorCode::Validation, "session is closed"));
}

support::ExpectedVoid AgentSessionRuntime::clear_follow_up_queue() {
    if (auto rejected = reject_if_closed(); !rejected) {
        return rejected;
    }
    return agent_ ? agent_->clear_follow_up_queue() : std::unexpected(support::make_error(
        support::ErrorCode::Validation, "session is closed"));
}

support::ExpectedVoid AgentSessionRuntime::clear_input_queues() {
    if (auto rejected = reject_if_closed(); !rejected) {
        return rejected;
    }
    return agent_ ? agent_->clear_input_queues() : std::unexpected(support::make_error(
        support::ErrorCode::Validation, "session is closed"));
}

support::Expected<std::string> AgentSessionRuntime::set_thinking_level(
    std::string_view level) {
    if (auto rejected = reject_if_closed(); !rejected) {
        return std::unexpected(rejected.error());
    }
    if (!agent_) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation, "session is closed"));
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
    // surface and are not resumable, so the facade drops the entry there
    // exactly like the creation-time `model_change` entry.
    if (auto appended = session_.store->append_thinking_level_change(
            std::nullopt, *effective);
        !appended) {
        return std::unexpected(std::move(appended.error()));
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

boost::asio::awaitable<support::ExpectedVoid> AgentSessionRuntime::set_model(
    ai::Model model) {
    if (auto rejected = reject_if_closed(); !rejected) {
        co_return std::unexpected(rejected.error());
    }
    if (!agent_ || !services_.model_runtime) {
        co_return std::unexpected(support::make_error(
            support::ErrorCode::Validation, "session is closed"));
    }

    // pi setModel: `if (!(await checkAuth(model.provider))) throw`.
    auto checked = co_await services_.model_runtime->check_auth(model.provider);
    if (!checked) {
        co_return std::unexpected(std::move(checked.error()));
    }
    if (!*checked) {
        co_return std::unexpected(support::make_error(
            support::ErrorCode::Auth,
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
[[nodiscard]] boost::asio::awaitable<support::ExpectedVoid>
AgentSessionRuntime::apply_model_switch(
    ai::Model model,
    std::string thinking_level) {
    if (auto swapped = agent_->set_model(std::move(model)); !swapped) {
        co_return std::unexpected(std::move(swapped.error()));
    }
    const auto& active_model = agent_->state().model;

    // Persist the `model_change` session entry (pi `appendModelChange`).
    // In-memory sessions have no v3 tree entry surface and are not resumable,
    // so the facade drops the entry there exactly like the creation-time
    // entry.
    if (auto appended = session_.store->append_model_change(
            std::nullopt, active_model.provider, active_model.id);
        !appended) {
        co_return std::unexpected(std::move(appended.error()));
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
    co_return support::ExpectedVoid{};
}

boost::asio::awaitable<support::Expected<std::optional<ModelCycleResult>>>
AgentSessionRuntime::cycle_model(std::string_view direction) {
    if (auto rejected = reject_if_closed(); !rejected) {
        co_return std::unexpected(rejected.error());
    }
    if (!agent_ || !services_.model_runtime) {
        co_return std::unexpected(support::make_error(
            support::ErrorCode::Validation, "session is closed"));
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

support::Expected<std::optional<std::string>>
AgentSessionRuntime::cycle_thinking_level() {
    if (auto rejected = reject_if_closed(); !rejected) {
        return std::unexpected(rejected.error());
    }
    if (!agent_) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation, "session is closed"));
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
[[nodiscard]] support::ExpectedVoid persist_leaf_marker(
    harness::session::SessionStore& store,
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

boost::asio::awaitable<support::ExpectedVoid>
AgentSessionRuntime::wait_for_idle() {
    // pi `waitForIdle`: settle when an Agent run is active. The run in
    // flight continues to its normal terminal; the settled signal is
    // cancelled exactly when the run settles (same waiter-before-cancel
    // ordering as PendingUserBashCommit). The wait itself cannot fail.
    if (!prompt_active_ || !prompt_settled_signal_) {
        co_return support::ExpectedVoid{};
    }
    boost::system::error_code wait_error;
    co_await prompt_settled_signal_->async_wait(
        boost::asio::redirect_error(
            boost::asio::use_awaitable, wait_error));
    co_return support::ExpectedVoid{};
}

support::Expected<coding_agent::SessionTreeTopology>
AgentSessionRuntime::session_tree() const {
    if (auto rejected = reject_if_closed(); !rejected) {
        return std::unexpected(rejected.error());
    }
    const auto store_path = session_.store ? session_.store->path() : std::nullopt;
    if (store_path) {
        // The store's live tree answers from memory; the session file is
        // never re-read for topology queries.
        return coding_agent::SessionTreeTopology{
            .roots = session_.store->tree(),
            .leaf_id = session_.store->leaf_id(),
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

support::Expected<coding_agent::TreeNavigationResult>
AgentSessionRuntime::navigate_tree(std::string_view target_id) {
    if (auto rejected = reject_if_closed(); !rejected) {
        return std::unexpected(rejected.error());
    }
    // pi's navigateTree streaming guard: the interactive flow aborts the
    // active response (then waits for settle) before navigating; a direct
    // call while a run is active is rejected verbatim (regression
    // tree-during-streaming).
    if (prompt_active_) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "Wait for the current response to finish before navigating the session tree."));
    }
    if (!agent_) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation, "session is closed"));
    }

    const auto store_path = session_.store ? session_.store->path() : std::nullopt;
    if (store_path) {
        auto& store = *session_.store;
        // pi: no-op when already at the target.
        if (target_id == store.leaf_id()) {
            return coding_agent::TreeNavigationResult{};
        }
        const auto target = store.get_entry(target_id);
        if (!target.has_value()) {
            return std::unexpected(support::make_error(
                support::ErrorCode::Session,
                std::format("Entry {} not found", target_id)));
        }

        const auto decision = tree_leaf_decision(
            store.effective_parent_id(target->entry_id), *target);

        // Persist the leaf marker; a successful marker append also moves
        // the live tree's leaf to the same position, while a failure
        // changes nothing — durable and live state never drift (Session
        // Event Commitment ordering: the durable mutation precedes the
        // live-state advance below).
        if (auto persisted =
                persist_leaf_marker(store, decision.new_leaf_id);
            !persisted) {
            return std::unexpected(persisted.error());
        }

        // Rebuild the live Agent context from the new path (pi
        // `agent.state.messages = sessionContext.messages`).
        const auto context = store.build_context();
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
        return std::unexpected(support::make_error(
            support::ErrorCode::Session,
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

support::ExpectedVoid AgentSessionRuntime::set_entry_label(
    std::string_view entry_id,
    std::optional<std::string> label) {
    if (auto rejected = reject_if_closed(); !rejected) {
        return std::unexpected(rejected.error());
    }
    const auto store_path = session_.store ? session_.store->path() : std::nullopt;
    if (!store_path) {
        // In-memory sessions keep no entry surface; the change is dropped
        // like every in-memory store write (the tree's own display keeps it
        // for the session's lifetime).
        return {};
    }
    if (!session_.store->get_entry(entry_id).has_value()) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Session,
            std::format("Entry {} not found", entry_id)));
    }
    // pi `appendLabelChange`: the label entry hangs under the current leaf
    // (null at the root position).
    std::optional<std::string> parent_id;
    if (auto leaf = session_.store->leaf_id(); !leaf.empty()) {
        parent_id = std::move(leaf);
    }
    return session_.store->append_label_change(
        std::move(parent_id), std::string{entry_id}, std::move(label));
}

boost::asio::awaitable<support::Expected<coding_agent::CompactionResult>>
AgentSessionRuntime::compact(std::string custom_instructions) {
    if (auto rejected = reject_if_closed(); !rejected) {
        co_return std::unexpected(rejected.error());
    }
    if (compaction_active_) {
        co_return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
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

    support::Expected<coding_agent::CompactionResult> result;
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
    try {
#endif
        result = co_await compact_impl(std::move(custom_instructions));
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
    } catch (const std::exception& error) {
        result = std::unexpected(support::make_error(
            support::ErrorCode::Unknown,
            "session compact coroutine failed",
            error.what()));
    } catch (...) {
        result = std::unexpected(support::make_error(
            support::ErrorCode::Unknown,
            "session compact coroutine failed"));
    }
#endif
    compaction_active_ = false;
    emit_session_event(CompactionEndEvent{
        .reason = "manual",
        .aborted = false,
        .error_message = result
            ? std::nullopt
            : std::optional<std::string>{result.error().message},
    });
    // A Close requested while the compaction was in flight finalizes here,
    // after the compaction reached its terminal outcome — never while the
    // compaction could still touch the Agent, the store, or the persistence
    // channel (issue #467).
    if (lifecycle_ == Lifecycle::Closing && !prompt_active_ &&
        !user_bash_active_) {
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
        support::ErrorCode::Validation,
        "No model selected.\n\nThen use /model to select a model.");
}

} // namespace

boost::asio::awaitable<support::Expected<coding_agent::CompactionResult>>
AgentSessionRuntime::compact_impl(std::string custom_instructions) {
    if (!agent_ || !session_.store) {
        co_return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "session Agent is unavailable"));
    }
    const auto model = agent_->state().model;
    if (model.id == agent::detail::kDefaultModel.id) {
        co_return std::unexpected(no_model_selected_error());
    }
    const auto store_path = session_.store ? session_.store->path() : std::nullopt;
    if (!store_path) {
        // In-memory sessions have no tree/entry surface: there is no session
        // file to persist a CompactionEntry into or to rebuild context from.
        co_return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "compaction requires a persisted session file"));
    }
    const auto session_path = *store_path;

    const auto settings = effective_compaction_settings();
    // The store's live tree answers the branch query from memory (pi
    // `SessionManager.getBranch`); the session file is not re-read.
    auto branch_entries = session_.store->get_branch();
    // pi SessionManager.getBranch is root-to-leaf with the leaf last; the C++
    // tree walks leaf-to-root, so reverse for the machinery.
    std::reverse(branch_entries.begin(), branch_entries.end());
    std::vector<const harness::session::SessionEntry*> branch;
    branch.reserve(branch_entries.size());
    for (const auto& entry : branch_entries) {
        branch.push_back(&entry);
    }

    auto preparation = harness::session::prepare_compaction(branch, settings);
    if (!preparation) {
        co_return std::unexpected(preparation.error());
    }
    if (!*preparation) {
        if (!branch.empty() &&
            branch.back()->kind == harness::session::SessionEntryKind::Compaction) {
            co_return std::unexpected(support::make_error(
                support::ErrorCode::Validation,
                "Already compacted"));
        }
        co_return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "Nothing to compact (session too small)"));
    }
    const auto& prep = **preparation;
    // pi AgentSession.compact refuses when neither history nor turn prefix has
    // messages to summarize (a session that fits the keepRecentTokens budget).
    if (prep.messages_to_summarize.empty() && prep.turn_prefix_messages.empty()) {
        co_return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "Nothing to compact (session too small)"));
    }

    co_return co_await execute_compaction(
        prep, settings, std::move(custom_instructions));
}

boost::asio::awaitable<support::Expected<coding_agent::CompactionResult>>
AgentSessionRuntime::execute_compaction(
    const harness::session::CompactionPreparation& preparation,
    const harness::session::CompactionSettings& settings,
    std::string custom_instructions) {
    if (!agent_ || !session_.store) {
        co_return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "session Agent is unavailable"));
    }
    const auto store_path = session_.store->path();
    if (!store_path) {
        // In-memory sessions have no tree/entry surface: there is no session
        // file to persist a CompactionEntry into or to rebuild context from.
        co_return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "compaction requires a persisted session file"));
    }
    const auto session_path = *store_path;
    const auto model = agent_->state().model;

    harness::session::SummarizationStreamFn summarization_stream =
        [factory = make_stream_factory(), model](
            ai::AiContext context,
            ai::SimpleStreamOptions options) mutable
            -> boost::asio::awaitable<support::Expected<ai::AssistantMessage>> {
        auto stream = factory(
            model, std::move(context), std::move(options));
        co_return co_await cch::ai::detail::await_async_result(
            std::move(stream).run({}));
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
    // The CompactionEntry must land after the run's message entries: drain
    // the persistence channel before this mid-run typed append (ADR 0040).
    if (persistence_) {
        co_await persistence_->drain();
    }
    if (auto appended = session_.store->append_compaction(
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

    // Rebuild the live context from the store's live tree exactly like pi
    // (`this.agent.state.messages = sessionContext.messages`): the next
    // prompt's model context is compactionSummary + retained tail. The
    // compaction append above already advanced the cached tree, so there is
    // nothing to re-read.
    auto context = session_.store->build_context();
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
    const auto store_path = session_.store->path();
    if (!store_path) {
        // In-memory sessions have no tree/entry surface (same confinement as
        // the manual trigger): auto-compaction is skipped silently.
        co_return false;
    }

    auto branch_entries = session_.store->get_branch();
    std::reverse(branch_entries.begin(), branch_entries.end());
    std::vector<const harness::session::SessionEntry*> branch;
    branch.reserve(branch_entries.size());
    for (const auto& entry : branch_entries) {
        branch.push_back(&entry);
    }

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

AgentSessionSnapshot AgentSessionRuntime::snapshot(
    const std::optional<std::filesystem::path>& session_path) const {
    return AgentSessionSnapshot{
        .agent_state = agent_ ? agent_->state() : agent::AgentState{},
        .metadata = session_.metadata,
        .topology = session_.topology,
        .session_path = session_path,
        .session_event_diagnostics = session_event_diagnostics_,
    };
}

std::size_t AgentSessionRuntime::message_count() const {
    return agent_ ? agent_->state().messages.size() : 0;
}

std::optional<std::string> AgentSessionRuntime::last_assistant_text() const {
    return agent_ ? last_assistant_text_from(agent_->state().messages) : std::nullopt;
}

std::optional<std::string> AgentSessionRuntime::session_name() const {
    if (!session_.store || !session_.store->path()) {
        // In-memory sessions have no `session_info` surface (pi
        // `getSessionName` walks the entries; the in-memory store keeps
        // none).
        return std::nullopt;
    }
    return session_.store->get_session_name();
}

support::Expected<std::optional<std::string>>
AgentSessionRuntime::set_session_name(std::string name) {
    // pi `appendSessionInfo` sanitization: CR/LF runs become one space,
    // then the result is trimmed.
    auto sanitized = runtime::sanitize_session_name(name);
    if (!session_.store || !session_.store->path()) {
        // In-memory sessions keep no `session_info` entry surface; the
        // change is dropped like every in-memory store write.
        return std::nullopt;
    }
    std::optional<std::string> parent_id;
    if (auto leaf = session_.store->leaf_id(); !leaf.empty()) {
        parent_id = std::move(leaf);
    }
    if (auto appended = session_.store->append_session_info(
            std::move(parent_id), sanitized);
        !appended) {
        return std::unexpected(appended.error());
    }
    return sanitized;
}

SessionStats AgentSessionRuntime::session_stats() const {
    SessionStats stats;
    // Persisted sessions aggregate over the store's entries (pi
    // `getEntries()`, so compacted-away history still counts); in-memory
    // sessions derive from the live context.
    std::vector<ai::MessageVariant> messages;
    if (session_.store && session_.store->path()) {
        for (const auto& entry : session_.store->entries()) {
            if (entry.kind != harness::session::SessionEntryKind::Message ||
                !entry.message) {
                continue;
            }
            messages.push_back(*entry.message);
        }
    } else if (agent_) {
        messages = agent_->state().messages;
    }
    for (const auto& message : messages) {
        ++stats.total_messages;
        if (std::holds_alternative<ai::UserMessage>(message)) {
            ++stats.user_messages;
        } else if (const auto* assistant =
                       std::get_if<ai::AssistantMessage>(&message)) {
            ++stats.assistant_messages;
            for (const auto& content : assistant->content) {
                if (std::holds_alternative<ai::ToolCallContent>(content)) {
                    ++stats.tool_calls;
                }
            }
            stats.input_tokens += assistant->usage.input;
            stats.output_tokens += assistant->usage.output;
            stats.cache_read += assistant->usage.cache_read;
            stats.cache_write += assistant->usage.cache_write;
        } else if (std::holds_alternative<ai::ToolResultMessage>(message)) {
            ++stats.tool_results;
        }
    }
    return stats;
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
    if (!prompt_active_ && !user_bash_active_ && !compaction_active_) {
        finalize_close();
    }
}

std::shared_ptr<harness::AsyncExecutionEnv>
AgentSessionRuntime::release_close_resources() noexcept {
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

    if (services_.env_owned) {
        return std::move(services_.env);
    }
    services_.env.reset();
    return {};
}

boost::asio::awaitable<void> AgentSessionRuntime::finalize_close_after_active_work() {
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
    auto owned_env = release_close_resources();
    if (owned_env) {
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
        try {
#endif
            (void)co_await ai::detail::await_async_result(owned_env->cleanup());
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
        } catch (...) {
            // cleanup() is best-effort and must not make close fallible.
        }
#endif
    }
    lifecycle_ = Lifecycle::Closed;
}

void AgentSessionRuntime::finalize_close() noexcept {
    if (lifecycle_ == Lifecycle::Closed) {
        return;
    }
    // Idle close: no prompt, User Bash, or compaction is admitted, so no
    // Session Event Commitment can still be in flight (submissions only
    // happen inside an admitted run whose settle drains the channel).
    auto owned_env = release_close_resources();
    lifecycle_ = Lifecycle::Closed;

    // Idle close has no host executor to await. Transfer the factory-owned
    // environment to a posted best-effort cleanup task on the session's
    // Runtime loop when one exists, so the final application Close drain
    // quiesces process resources before the loop stops (ADR 0040, issue
    // #467); a session assembled without a Runtime root falls back to the
    // shared system executor. Host-owned environments were detached above
    // without cleanup().
    if (owned_env) {
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
        try {
#endif
            // post() prevents a cleanup coroutine from executing inline on the
            // close() stack before its first suspension point.
            const auto cleanup_executor = services_.runtime_target
                ? services_.runtime_target->executor()
                : boost::asio::any_io_executor{boost::asio::system_executor{}};
            boost::asio::post(
                cleanup_executor,
                [env = std::move(owned_env), cleanup_executor]() mutable {
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
                    try {
#endif
                        boost::asio::co_spawn(
                            cleanup_executor,
                            [env = std::move(env)]() -> boost::asio::awaitable<void> {
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
                                try {
#endif
                                    (void)co_await ai::detail::await_async_result(env->cleanup());
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

} // namespace cch::coding_agent::runtime

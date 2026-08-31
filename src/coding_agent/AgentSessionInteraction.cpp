#include "coding_agent/AgentSessionImpl.hpp"

#include <cch/ai/Content.hpp>
#include <cch/coding_agent/AgentConfigDir.hpp>
#include <cch/coding_agent/ProjectTrust.hpp>
#include <cch/coding_agent/Settings.hpp>
#include <cch/agent/harness/FileSystem.hpp>
#include <cch/agent/harness/session/SessionStore.hpp>
#include <cch/agent/harness/session/SessionTree.hpp>

#include "agent/AgentMessageAccess.hpp"
#include "support/AsyncResultBridge.hpp"
#include "ai/ModelThinkingLevel.hpp"
#include "coding_agent/BoundedText.hpp"
#include "coding_agent/ProjectResourceLoader.hpp"
#include "coding_agent/runtime/AgentSessionInteractiveAccess.hpp"
#include "coding_agent/runtime/UserBashOutputAccumulator.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <chrono>
#include <exception>
#include <format>
#include <optional>
#include <utility>

namespace cch::coding_agent {
namespace {

[[nodiscard]] std::optional<std::string> last_assistant_text_from(const std::vector<ai::MessageVariant>& history) {
    for (auto it = history.rbegin(); it != history.rend(); ++it) {
        if (const auto* am = std::get_if<ai::AssistantMessage>(&*it)) {
            return ai::text_from_assistant_content(am->content);
        }
    }
    return std::nullopt;
}

[[nodiscard]] ai::TimestampMs completion_timestamp_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();
}

[[nodiscard]] ai::BashExecutionMessage make_bash_execution_message(const runtime::UserShellResult& result,
        const runtime::UserBashOutputAccumulator& output,
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

boost::asio::awaitable<support::Expected<AgentSessionReloadResult>> AgentSession::Impl::reload() {
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
                support::ErrorCode::Validation, "session has no retained resource loading request"));
    }
    if (!config_.resource_file_systems.workspace) {
        co_return std::unexpected(support::make_error(
                support::ErrorCode::Workspace, "reload failed: resource filesystem capability is unavailable"));
    }
    auto resource_filesystems = config_.resource_file_systems;
    auto resource_request = *config_.resource_loading_request;
    // pi `reload()` preserves `SettingsManager.projectTrusted`: the reload
    // re-runs with the current trust state, never re-resolving it.
    resource_request.project_trust_override = is_project_trusted();
    resource_request.workspace = session_.workspace;
    ProjectTrustStore trust_store{trust_store_file_path()};
    auto loading = co_await support::detail::await_async_result(
            load_project_resources(std::move(resource_filesystems), trust_store, std::move(resource_request), {}));
    if (!loading) {
        co_return std::unexpected(harness::to_util_error(std::move(loading.error())));
    }
    if (!loading->fatal_errors.empty()) {
        co_return std::unexpected(support::make_error(
                support::ErrorCode::Validation, "reload failed", loading->fatal_errors.front().message));
    }

    // Swap the live resource snapshots and the System Prompt inputs (pi
    // `_rebuildSystemPrompt` reads the fresh loader results).
    skills_ = std::move(loading->resources.skills);
    templates_ = std::move(loading->resources.prompt_templates);
    config_.custom_prompt = std::move(loading->resources.system_prompt);
    config_.append_system_prompt = std::move(loading->resources.append_system_prompt);
    config_.context_files = std::move(loading->resources.agents_files);
    config_.system_prompt_source = std::move(loading->resources.system_prompt_source);
    config_.append_system_prompt_sources = std::move(loading->resources.append_system_prompt_sources);
    config_.skill_diagnostics = std::move(loading->skill_diagnostics);
    config_.prompt_diagnostics = std::move(loading->prompt_diagnostics);
    config_.theme_diagnostics = std::move(loading->theme_diagnostics);

    // Rebuild the System Prompt and push it into the live Agent (pi
    // `_rebuildSystemPrompt` → `agent.state.systemPrompt`).
    if (!agent_) {
        co_return std::unexpected(support::make_error(support::ErrorCode::Validation, "session Agent is unavailable"));
    }
    agent_->set_system_prompt(rebuild_system_prompt());

    AgentSessionReloadResult result;
    result.skill_diagnostics = config_.skill_diagnostics;
    result.prompt_diagnostics = config_.prompt_diagnostics;
    result.theme_diagnostics = config_.theme_diagnostics;
    result.themes = std::move(loading->resources.themes);
    co_return result;
}

void AgentSession::Impl::refresh_bash_session_environment() {
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
            state.thinking_level.empty() ? std::nullopt : std::optional<std::string>{state.thinking_level};
}

support::ExpectedVoid AgentSession::Impl::commit_user_bash_completion(runtime::UserBashCompletion& completion) {
    // Live Session State advances first; a Session Store failure is reported
    // on the completion diagnostic without rolling the message back.
    if (auto committed = agent::detail::AgentMessageAccess::append_bash_execution(*agent_, completion.message);
            !committed) {
        return std::unexpected(std::move(committed.error()));
    }
    if (auto persisted = session_.store->append(ai::MessageVariant{completion.message}); !persisted) {
        completion.diagnostic = std::move(persisted.error());
    }
    return {};
}

void AgentSession::Impl::flush_pending_user_bash() {
    if (pending_user_bash_.empty()) return;

    auto pending = std::exchange(pending_user_bash_, {});
    for (auto& entry : pending) {
        if (agent_ && session_.store) {
            entry->commit_result = commit_user_bash_completion(entry->completion);
        } else {
            entry->commit_result = std::unexpected(
                    support::make_error(support::ErrorCode::Validation, "session Agent is unavailable"));
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

boost::asio::awaitable<support::Expected<runtime::UserBashCompletion>> AgentSession::Impl::run_user_bash(
        std::string command, bool exclude_from_context, runtime::UserBashProgressSink progress_sink) {
    if (auto rejected = reject_if_closed(); !rejected) {
        co_return std::unexpected(rejected.error());
    }
    if (auto rejected = reject_if_user_bash_busy(); !rejected) {
        co_return std::unexpected(rejected.error());
    }
    if (!services_.user_shell) {
        co_return std::unexpected(support::make_error(support::ErrorCode::Validation, "User Shell is unavailable"));
    }

    user_bash_active_ = true;
    active_user_bash_stop_source_.emplace();
    const auto recorded_command = command;
    support::Expected<runtime::UserShellResult> shell_result =
            std::unexpected(support::make_error(support::ErrorCode::Unknown, "User Shell execution did not finish"));
    if (progress_sink) {
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
        try {
#endif
            if (auto started = progress_sink(runtime::UserBashProgress{
                        .command = recorded_command,
                        .output = {},
                        .exclude_from_context = exclude_from_context,
                        .exit_code = {},
                        .full_output_path = {},
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
            co_return std::unexpected(support::make_error(support::ErrorCode::Unknown,
                    "User Bash progress callback failed",
                    bounded_redacted_presentation(error.what())));
        } catch (...) {
            active_user_bash_stop_source_.reset();
            user_bash_active_ = false;
            co_return std::unexpected(
                    support::make_error(support::ErrorCode::Unknown, "User Bash progress callback failed"));
        }
#endif
    }
    runtime::UserBashOutputAccumulator output;
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
    try {
#endif
        shell_result = co_await support::detail::await_async_result(services_.user_shell->execute(
                std::move(command),
                [recorded_command, exclude_from_context, &output, &progress_sink](
                        std::string_view update) -> support::ExpectedVoid {
                    output.append(update);
                    if (!progress_sink) return {};
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
                    try {
#endif
                        return progress_sink(runtime::UserBashProgress{
                                .command = recorded_command,
                                .output = output.tail(),
                                .exclude_from_context = exclude_from_context,
                                .exit_code = {},
                                .full_output_path = {},
                        });
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
                    } catch (const std::exception& error) {
                        return std::unexpected(support::make_error(
                                support::ErrorCode::Unknown, "User Bash progress callback failed", error.what()));
                    } catch (...) {
                        return std::unexpected(
                                support::make_error(support::ErrorCode::Unknown, "User Bash progress callback failed"));
                    }
#endif
                },
                active_user_bash_stop_source_->get_token()));
        if (shell_result) {
            output.finish();
        }
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
    } catch (const std::exception& error) {
        shell_result = std::unexpected(
                support::make_error(support::ErrorCode::Unknown, "User Shell execution failed", error.what()));
    } catch (...) {
        shell_result = std::unexpected(
                support::make_error(support::ErrorCode::Unknown, "User Shell execution failed", "unknown exception"));
    }
#endif

    active_user_bash_stop_source_.reset();
    user_bash_active_ = false;
    const auto finalize_if_last_active_work = [this]() -> boost::asio::awaitable<void> {
        if (lifecycle_ == Lifecycle::Closing && !prompt_active_ && !compaction_active_) {
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
            *shell_result, output, recorded_command, exclude_from_context, completion_timestamp_ms());

    runtime::UserBashCompletion completion{
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
                .committed_signal = boost::asio::steady_timer(co_await boost::asio::this_coro::executor),
                .commit_result = {},
        });
        pending->committed_signal.expires_at(std::chrono::steady_clock::time_point::max());
        pending_user_bash_.push_back(pending);
        if (progress_sink) {
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
            try {
#endif
                (void)progress_sink(runtime::UserBashProgress{
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
        co_return std::unexpected(support::make_error(support::ErrorCode::Validation, "session Agent is unavailable"));
    }
    if (auto committed = commit_user_bash_completion(completion); !committed) {
        co_await finalize_if_last_active_work();
        co_return std::unexpected(committed.error());
    }
    co_await finalize_if_last_active_work();
    co_return completion;
}

void AgentSession::Impl::cancel_user_bash() {
    if (user_bash_active_ && active_user_bash_stop_source_) {
        (void)active_user_bash_stop_source_->request_stop();
    }
}

support::ExpectedVoid AgentSession::Impl::steer(
        std::string text, std::vector<ai::ImageContent> images, bool expand_prompt_templates) {
    if (auto rejected = reject_if_closed(); !rejected) {
        return rejected;
    }
    auto message = detail::make_admitted_user_message(
            std::move(text), skills_, templates_, std::move(images), expand_prompt_templates);
    return agent_->steer(ai::MessageVariant{std::move(message)});
}

support::ExpectedVoid AgentSession::Impl::follow_up(
        std::string text, std::vector<ai::ImageContent> images, bool expand_prompt_templates) {
    if (auto rejected = reject_if_closed(); !rejected) {
        return rejected;
    }
    auto message = detail::make_admitted_user_message(
            std::move(text), skills_, templates_, std::move(images), expand_prompt_templates);
    return agent_->follow_up(ai::MessageVariant{std::move(message)});
}

support::ExpectedVoid AgentSession::Impl::set_steering_mode(agent::InputQueueMode mode) {
    if (auto rejected = reject_if_closed(); !rejected) {
        return rejected;
    }
    return agent_ ? agent_->set_steering_mode(mode)
                  : std::unexpected(support::make_error(support::ErrorCode::Validation, "session is closed"));
}

support::ExpectedVoid AgentSession::Impl::set_follow_up_mode(agent::InputQueueMode mode) {
    if (auto rejected = reject_if_closed(); !rejected) {
        return rejected;
    }
    return agent_ ? agent_->set_follow_up_mode(mode)
                  : std::unexpected(support::make_error(support::ErrorCode::Validation, "session is closed"));
}

support::ExpectedVoid AgentSession::Impl::clear_steering_queue() {
    if (auto rejected = reject_if_closed(); !rejected) {
        return rejected;
    }
    return agent_ ? agent_->clear_steering_queue()
                  : std::unexpected(support::make_error(support::ErrorCode::Validation, "session is closed"));
}

support::ExpectedVoid AgentSession::Impl::clear_follow_up_queue() {
    if (auto rejected = reject_if_closed(); !rejected) {
        return rejected;
    }
    return agent_ ? agent_->clear_follow_up_queue()
                  : std::unexpected(support::make_error(support::ErrorCode::Validation, "session is closed"));
}

support::ExpectedVoid AgentSession::Impl::clear_input_queues() {
    if (auto rejected = reject_if_closed(); !rejected) {
        return rejected;
    }
    return agent_ ? agent_->clear_input_queues()
                  : std::unexpected(support::make_error(support::ErrorCode::Validation, "session is closed"));
}

support::Expected<std::string> AgentSession::Impl::set_thinking_level(std::string_view level) {
    if (auto rejected = reject_if_closed(); !rejected) {
        return std::unexpected(rejected.error());
    }
    if (!agent_) {
        return std::unexpected(support::make_error(support::ErrorCode::Validation, "session is closed"));
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
    if (auto appended = session_.store->append_thinking_level_change(std::nullopt, *effective); !appended) {
        return std::unexpected(std::move(appended.error()));
    }

    // Persist the settings default unless the active model supports no
    // thinking and the level is "off" (pi agent-session.ts setThinkingLevel:
    // `if (this.supportsThinking() || effectiveLevel !== "off")`).
    const auto& active_model = agent_->state().model;
    if (services_.settings_manager && (active_model.reasoning || *effective != "off")) {
        if (auto saved = services_.settings_manager->set_default_thinking_level(SettingsScope::Global, *effective);
                !saved) {
            return std::unexpected(std::move(saved.error()));
        }
    }
    return effective;
}

boost::asio::awaitable<support::ExpectedVoid> AgentSession::Impl::set_model(ai::Model model) {
    if (auto rejected = reject_if_closed(); !rejected) {
        co_return std::unexpected(rejected.error());
    }
    if (!agent_ || !services_.model_runtime) {
        co_return std::unexpected(support::make_error(support::ErrorCode::Validation, "session is closed"));
    }

    // pi setModel: `if (!(await checkAuth(model.provider))) throw`.
    auto checked = co_await support::detail::await_async_result(services_.model_runtime->check_auth(model.provider));
    if (!checked) {
        co_return std::unexpected(std::move(checked.error()));
    }
    if (!*checked) {
        co_return std::unexpected(
                support::make_error(support::ErrorCode::Auth, "No API key for " + model.provider + "/" + model.id));
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
[[nodiscard]] std::string AgentSession::Impl::resolve_thinking_level_for_switch(
        const std::optional<std::string>& explicit_level) const {
    if (explicit_level) return *explicit_level;
    if (!agent_ || !agent_->state().model.reasoning) {
        return services_.settings_manager && services_.settings_manager->settings().default_thinking_level
                       ? *services_.settings_manager->settings().default_thinking_level
                       : "medium";
    }
    return agent_->state().thinking_level;
}

/// pi `_cycleScopedModel`/`_cycleAvailableModel` shared tail: apply the model
/// (pi `agent.state.model = model`), append the `model_change` entry, write
/// the global settings default, and re-clamp the thinking level — the same
/// persistence sequence as `set_model`.
[[nodiscard]] boost::asio::awaitable<support::ExpectedVoid> AgentSession::Impl::apply_model_switch(
        ai::Model model, std::string thinking_level) {
    if (auto swapped = agent_->set_model(std::move(model)); !swapped) {
        co_return std::unexpected(std::move(swapped.error()));
    }
    const auto& active_model = agent_->state().model;

    // Persist the `model_change` session entry (pi `appendModelChange`).
    // In-memory sessions have no v3 tree entry surface and are not resumable,
    // so the facade drops the entry there exactly like the creation-time
    // entry.
    if (auto appended = session_.store->append_model_change(std::nullopt, active_model.provider, active_model.id);
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

boost::asio::awaitable<support::Expected<std::optional<ModelCycleResult>>> AgentSession::Impl::cycle_model(
        std::string_view direction) {
    if (auto rejected = reject_if_closed(); !rejected) {
        co_return std::unexpected(rejected.error());
    }
    if (!agent_ || !services_.model_runtime) {
        co_return std::unexpected(support::make_error(support::ErrorCode::Validation, "session is closed"));
    }
    const bool forward = direction != "backward";

    // Scoped path (pi `_cycleScopedModel`): filter the scoped set by
    // configured auth, then cycle within it.
    if (!scoped_models_.empty()) {
        std::vector<ScopedModel> eligible;
        for (const auto& scoped : scoped_models_) {
            auto checked = co_await support::detail::await_async_result(
                    services_.model_runtime->check_auth(scoped.model.provider));
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
            if (eligible[index].model.provider == current.provider && eligible[index].model.id == current.id) {
                current_index = index;
                break;
            }
        }
        const auto next_index = forward ? (current_index + 1) % eligible.size()
                                        : (current_index + eligible.size() - 1) % eligible.size();
        const auto& next = eligible[next_index];
        const auto thinking_level = resolve_thinking_level_for_switch(next.thinking_level);
        if (auto applied = co_await apply_model_switch(next.model, thinking_level); !applied) {
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
    auto available = co_await support::detail::await_async_result(services_.model_runtime->get_available());
    if (!available) {
        co_return std::unexpected(std::move(available.error()));
    }
    if (available->size() <= 1) {
        co_return std::optional<ModelCycleResult>{};
    }

    const auto& current = agent_->state().model;
    std::size_t current_index = 0;
    for (std::size_t index = 0; index < available->size(); ++index) {
        if ((*available)[index].provider == current.provider && (*available)[index].id == current.id) {
            current_index = index;
            break;
        }
    }
    const auto next_index = forward ? (current_index + 1) % available->size()
                                    : (current_index + available->size() - 1) % available->size();
    const auto& next_model = (*available)[next_index];
    const auto thinking_level = resolve_thinking_level_for_switch(std::nullopt);
    if (auto applied = co_await apply_model_switch(next_model, thinking_level); !applied) {
        co_return std::unexpected(std::move(applied.error()));
    }
    co_return ModelCycleResult{
            .model = agent_->state().model,
            .thinking_level = agent_->state().thinking_level,
            .is_scoped = false,
    };
}

support::Expected<std::optional<std::string>> AgentSession::Impl::cycle_thinking_level() {
    if (auto rejected = reject_if_closed(); !rejected) {
        return std::unexpected(rejected.error());
    }
    if (!agent_) {
        return std::unexpected(support::make_error(support::ErrorCode::Validation, "session is closed"));
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
    const auto next_index = (current_index + 1) % static_cast<std::ptrdiff_t>(levels.size());
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

void AgentSession::Impl::set_scoped_models(std::vector<ScopedModel> models) { scoped_models_ = std::move(models); }

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
[[nodiscard]] std::string custom_message_text(const harness::session::CustomMessageEntryValue& value) {
    if (const auto* text = std::get_if<std::string>(&value.content)) {
        return *text;
    }
    std::string result;
    for (const auto& block : std::get<std::vector<harness::session::CustomMessageEntryContentBlock>>(value.content)) {
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
        std::optional<std::string> effective_parent, const harness::session::SessionEntry& entry) {
    if (entry.kind == harness::session::SessionEntryKind::Message && entry.message.has_value()) {
        if (const auto* user = std::get_if<ai::UserMessage>(&*entry.message)) {
            auto text = user_message_text(*user);
            return TreeLeafDecision{
                    .new_leaf_id = std::move(effective_parent),
                    .editor_text = std::move(text),
            };
        }
    } else if (entry.kind == harness::session::SessionEntryKind::CustomMessage) {
        if (const auto* value = std::get_if<harness::session::CustomMessageEntryValue>(&entry.value)) {
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
        harness::session::SessionStore& store, const std::optional<std::string>& new_leaf_id) {
    return store.append_leaf(std::nullopt, new_leaf_id);
}

} // namespace

support::Expected<SessionTreeTopology> AgentSession::Impl::session_tree() const {
    if (auto rejected = reject_if_closed(); !rejected) {
        return std::unexpected(rejected.error());
    }
    if (!session_.store) {
        return std::unexpected(support::make_error(support::ErrorCode::Validation, "session store is unavailable"));
    }
    // The store's live tree answers from memory for both persistence
    // alternatives; the session file is never re-read for topology queries.
    // The snapshot takes roots and leaf under the same lock, so a concurrent
    // append can never split the pair.
    const auto snapshot = session_.store->tree_snapshot();
    return SessionTreeTopology{
            .roots = snapshot.roots,
            .leaf_id = snapshot.leaf_id,
    };
}

support::Expected<TreeNavigationResult> AgentSession::Impl::navigate_tree(std::string_view target_id) {
    if (auto rejected = reject_if_closed(); !rejected) {
        return std::unexpected(rejected.error());
    }
    // pi's navigateTree streaming guard: the interactive flow aborts the
    // active response (then waits for settle) before navigating; a direct
    // call while a run is active is rejected verbatim (regression
    // tree-during-streaming).
    if (prompt_active_) {
        return std::unexpected(support::make_error(support::ErrorCode::Validation,
                "Wait for the current response to finish before navigating the session tree."));
    }
    if (!agent_) {
        return std::unexpected(support::make_error(support::ErrorCode::Validation, "session is closed"));
    }
    if (!session_.store) {
        return std::unexpected(support::make_error(support::ErrorCode::Validation, "session store is unavailable"));
    }

    // One path for both persistence alternatives: the store's live tree
    // carries the in-memory session's entries exactly like a persisted
    // session's (pi's non-persisting SessionManager keeps the same
    // in-memory entries).
    auto& store = *session_.store;
    // pi: no-op when already at the target.
    if (target_id == store.leaf_id()) {
        return TreeNavigationResult{};
    }
    const auto target = store.get_entry(target_id);
    if (!target.has_value()) {
        return std::unexpected(
                support::make_error(support::ErrorCode::Session, std::format("Entry {} not found", target_id)));
    }

    const auto decision = tree_leaf_decision(store.effective_parent_id(target->entry_id), *target);

    // Append the leaf marker; a successful marker append also moves the
    // live tree's leaf to the same position, while a failure changes
    // nothing — durable and live state never drift (Session Event
    // Commitment ordering: the durable mutation precedes the live-state
    // advance below). In-memory sessions mirror the same marker into the
    // live tree without disk I/O; at the root position the marker also
    // carries the linear-chain break (pi `resetLeaf`).
    if (auto persisted = persist_leaf_marker(store, decision.new_leaf_id); !persisted) {
        return std::unexpected(persisted.error());
    }

    // Rebuild the live Agent context from the new path (pi
    // `agent.state.messages = sessionContext.messages`).
    const auto context = store.build_context();
    if (auto replaced = agent::detail::AgentMessageAccess::replace_messages(*agent_, context.messages); !replaced) {
        return std::unexpected(replaced.error());
    }
    return TreeNavigationResult{
            .editor_text = decision.editor_text,
            .cancelled = false,
    };
}

support::ExpectedVoid AgentSession::Impl::set_entry_label(std::string_view entry_id, std::optional<std::string> label) {
    if (auto rejected = reject_if_closed(); !rejected) {
        return std::unexpected(rejected.error());
    }
    if (!session_.store) {
        return std::unexpected(support::make_error(support::ErrorCode::Validation, "session store is unavailable"));
    }
    if (!session_.store->get_entry(entry_id).has_value()) {
        return std::unexpected(
                support::make_error(support::ErrorCode::Session, std::format("Entry {} not found", entry_id)));
    }
    // pi `appendLabelChange`: the label entry hangs under the current leaf
    // (null at the root position).
    std::optional<std::string> parent_id;
    if (auto leaf = session_.store->leaf_id(); !leaf.empty()) {
        parent_id = std::move(leaf);
    }
    return session_.store->append_label_change(std::move(parent_id), std::string{entry_id}, std::move(label));
}

AgentSessionSnapshot AgentSession::Impl::snapshot() const {
    return AgentSessionSnapshot{
            .agent_state = agent_ ? agent_->state() : agent::AgentState{},
            .metadata = session_.metadata,
            .topology = session_.topology,
            .session_path = session_path_,
            .session_event_diagnostics = session_event_diagnostics_,
    };
}

std::size_t AgentSession::Impl::message_count() const { return agent_ ? agent_->state().messages.size() : 0; }

std::optional<std::string> AgentSession::Impl::last_assistant_text() const {
    return agent_ ? last_assistant_text_from(agent_->state().messages) : std::nullopt;
}

std::optional<std::string> AgentSession::Impl::session_name() const {
    if (!session_.store || !session_.store->path()) {
        // In-memory sessions have no `session_info` surface (pi
        // `getSessionName` walks the entries; the in-memory store keeps
        // none).
        return std::nullopt;
    }
    return session_.store->get_session_name();
}

support::Expected<std::optional<std::string>> AgentSession::Impl::set_session_name(std::string name) {
    // pi `appendSessionInfo` sanitization: CR/LF runs become one space,
    // then the result is trimmed.
    auto sanitized = runtime::sanitize_session_name(name);
    if (!session_.store || !session_.store->path()) {
        // The `session_info` surface stays scoped to persisted sessions;
        // the in-memory change is dropped.
        return std::nullopt;
    }
    std::optional<std::string> parent_id;
    if (auto leaf = session_.store->leaf_id(); !leaf.empty()) {
        parent_id = std::move(leaf);
    }
    if (auto appended = session_.store->append_session_info(std::move(parent_id), sanitized); !appended) {
        return std::unexpected(appended.error());
    }
    return sanitized;
}

runtime::SessionStats AgentSession::Impl::session_stats() const {
    runtime::SessionStats stats;
    // Persisted sessions aggregate over the store's entries (pi
    // `getEntries()`, so compacted-away history still counts); in-memory
    // sessions derive from the live context.
    std::vector<ai::MessageVariant> messages;
    if (session_.store && session_.store->path()) {
        for (const auto& entry : session_.store->entries()) {
            if (entry.kind != harness::session::SessionEntryKind::Message || !entry.message) {
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
        } else if (const auto* assistant = std::get_if<ai::AssistantMessage>(&message)) {
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

const std::vector<Skill>& AgentSession::Impl::skills() const { return skills_; }

const std::vector<PromptTemplate>& AgentSession::Impl::templates() const { return templates_; }

// ── Lazy-coroutine session entries ──────────────────────────────────────────

boost::asio::awaitable<support::ExpectedVoid> detail::session_set_model(
        std::shared_ptr<AgentSession::Impl> impl, ai::Model model) {
    if (!impl) {
        co_return std::unexpected(support::make_error(support::ErrorCode::Validation, "session is not initialized"));
    }
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
    try {
#endif
        co_return co_await impl->set_model(std::move(model));
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
    } catch (const std::exception& error) {
        co_return std::unexpected(
                support::make_error(support::ErrorCode::Unknown, "session set_model coroutine failed", error.what()));
    } catch (...) {
        co_return std::unexpected(
                support::make_error(support::ErrorCode::Unknown, "session set_model coroutine failed"));
    }
#endif
}

boost::asio::awaitable<support::Expected<std::optional<ModelCycleResult>>> detail::session_cycle_model(
        std::shared_ptr<AgentSession::Impl> impl, std::string direction) {
    if (!impl) {
        co_return std::unexpected(support::make_error(support::ErrorCode::Validation, "session is not initialized"));
    }
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
    try {
#endif
        co_return co_await impl->cycle_model(std::move(direction));
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
    } catch (const std::exception& error) {
        co_return std::unexpected(
                support::make_error(support::ErrorCode::Unknown, "session cycle_model coroutine failed", error.what()));
    } catch (...) {
        co_return std::unexpected(
                support::make_error(support::ErrorCode::Unknown, "session cycle_model coroutine failed"));
    }
#endif
}

boost::asio::awaitable<support::Expected<AgentSessionReloadResult>> detail::session_reload(
        std::shared_ptr<AgentSession::Impl> impl) {
    if (!impl) {
        co_return std::unexpected(support::make_error(support::ErrorCode::Validation, "session is not initialized"));
    }
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
    try {
#endif
        co_return co_await impl->reload();
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
    } catch (const std::exception& error) {
        co_return std::unexpected(
                support::make_error(support::ErrorCode::Unknown, "session reload coroutine failed", error.what()));
    } catch (...) {
        co_return std::unexpected(support::make_error(support::ErrorCode::Unknown, "session reload coroutine failed"));
    }
#endif
}

// ── AgentSessionInteractiveAccess (the one private Native TUI seam) ─────────

bool detail::AgentSessionInteractiveAccess::has_user_shell(const AgentSession& session) {
    return session.impl_ && session.impl_->has_user_shell();
}

bool detail::AgentSessionInteractiveAccess::is_project_trusted(const AgentSession& session) {
    return session.impl_ && session.impl_->is_project_trusted();
}

boost::asio::awaitable<support::Expected<runtime::UserBashCompletion>>
detail::AgentSessionInteractiveAccess::run_user_bash(AgentSession& session,
        std::string command,
        bool exclude_from_context,
        runtime::UserBashProgressSink progress_sink) {
    return run_user_bash_impl(session.impl_, std::move(command), exclude_from_context, std::move(progress_sink));
}

boost::asio::awaitable<support::Expected<runtime::UserBashCompletion>>
detail::AgentSessionInteractiveAccess::run_user_bash_impl(std::shared_ptr<AgentSession::Impl> impl,
        std::string command,
        bool exclude_from_context,
        runtime::UserBashProgressSink progress_sink) {
    if (!impl) {
        co_return std::unexpected(support::make_error(support::ErrorCode::Validation, "session is not initialized"));
    }
    co_return co_await impl->run_user_bash(std::move(command), exclude_from_context, std::move(progress_sink));
}

void detail::AgentSessionInteractiveAccess::cancel_user_bash(AgentSession& session) {
    if (session.impl_) {
        session.impl_->cancel_user_bash();
    }
}

} // namespace cch::coding_agent

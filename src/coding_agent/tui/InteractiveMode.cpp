// InteractiveMode: the public Native TUI entries plus the InteractiveEngine
// input-event pumping unit (#506): view-action admission and dispatch, slash
// routing binding, submission routing, interrupt admission, and the
// prompt/User Bash lifecycle. See InteractiveEngine.hpp for the unit map.

#include "InteractiveMode.hpp"

#include "support/AsyncResultBridge.hpp"
#include "coding_agent/runtime/AgentSessionInteractiveAccess.hpp"
#include "coding_agent/tui/InteractiveEngine.hpp"
#include "coding_agent/tui/ClipboardPaste.hpp"
#include "coding_agent/tui/ErrorPresentation.hpp"
#include "coding_agent/tui/ExternalEditor.hpp"
#include "coding_agent/tui/InteractiveView.hpp"
#include "coding_agent/tui/ModelFlowController.hpp"
#include "coding_agent/tui/SessionFlowController.hpp"
#include "coding_agent/tui/SessionUiBinding.hpp"
#include "coding_agent/tui/SettingsFlowController.hpp"
#include "coding_agent/tui/SuspendController.hpp"

#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace cch::coding_agent::tui {
namespace {

// ── Focused-editor User Bash syntax (ADR 0026) ────────────────────────────
// Folded from the deleted UserBashSyntax module: only a direct focused
// Native TUI editor submission interprets the `!`/`!!` prefixes.

struct UserBashInvocation {
    std::string command;
    bool exclude_from_context{false};
};

// The shared pure helpers used by the pumping path live in
// InteractiveView.hpp's detail namespace.
using interactive_view_detail::trim_editor_submission;

/// Parses one trimmed submission as User Bash. `!` runs with later model
/// context; `!!` runs excluded from model conversion; `!!!foo` is excluded
/// User Bash running `!foo`. A bare `!` or `!!` yields no invocation and
/// falls through to an ordinary Agent Prompt.
[[nodiscard]] std::optional<UserBashInvocation> parse_user_bash_invocation(
    std::string text) {
    text = trim_editor_submission(std::move(text));
    if (!text.starts_with('!')) return std::nullopt;
    const bool excluded = text.starts_with("!!");
    auto command = trim_editor_submission(text.substr(excluded ? 2 : 1));
    if (command.empty()) return std::nullopt;
    return UserBashInvocation{
        .command = std::move(command),
        .exclude_from_context = excluded,
    };
}

} // namespace

void InteractiveEngine::post_view_action(ViewAction action) {
    const auto weak = weak_from_this();
    const auto prompt_generation = generation();
    boost::asio::post(
        executor_,
        [weak, prompt_generation, action = std::move(action)]() mutable {
            if (const auto self = weak.lock()) {
                self->dispatch_view_action(std::move(action), prompt_generation);
            }
        });
}

void InteractiveEngine::dispatch_view_action(ViewAction action, std::size_t prompt_generation) {
    std::visit(
        [this, prompt_generation](auto&& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, InterruptAction>) {
                request_interrupt(prompt_generation, value.request);
            } else {
                dispatch(std::move(value));
            }
        },
        std::move(action));
}

void InteractiveEngine::dispatch(SubmitAction action) {
    submit(
        std::move(action.request.text),
        action.submission,
        {},
        SubmissionOrigin::FocusedEditor,
        action.request.editor_revision);
}

void InteractiveEngine::dispatch(ClipboardPasteAction) {
    if (clipboard_reader_ == nullptr || clipboard_read_active_) return;
    clipboard_read_active_ = true;
    const auto self = shared_from_this();
    auto bridged = support::detail::make_async_result_on(
            executor_, [self]() mutable -> boost::asio::awaitable<support::ExpectedVoid> {
                auto content = co_await read_clipboard_insert_content(*self->clipboard_reader_);
                if (content && !content->empty() && self->running_ && self->view_ != nullptr) {
                    self->view_->insert_editor_text(std::move(*content));
                    self->tui_.invalidate();
                }
                co_return support::ExpectedVoid{};
            });
    std::move(bridged).start(
        [weak = weak_from_this()](support::ExpectedVoid result) noexcept {
            const auto engine = weak.lock();
            if (!engine) return;
            engine->clipboard_read_active_ = false;
            // Baseline clipboard failures are intentionally silent; the
            // private bridge already mapped a launch failure to the same
            // generic outcome.
            (void)result;
        });
}

void InteractiveEngine::dispatch(DequeueAction) {
    dequeue_pending_input(true);
}

void InteractiveEngine::dispatch(ExitAction) {
    request_exit();
}

void InteractiveEngine::dispatch(CycleModelAction action) {
    model_flows_->cycle_model(
        action.direction == ModelCycleDirection::Forward ? "forward" : "backward");
}

void InteractiveEngine::dispatch(CycleThinkingAction) {
    settings_flows_->cycle_thinking_level();
}

void InteractiveEngine::dispatch(ToggleThinkingAction) {
    settings_flows_->toggle_thinking_block_visibility();
}

void InteractiveEngine::dispatch(SelectModelAction) {
    model_flows_->show_model_selector(std::nullopt);
}

void InteractiveEngine::dispatch(ResumeSessionAction) {
    session_flows_->open_resume();
}

void InteractiveEngine::dispatch(ForkSessionAction) {
    session_flows_->open_fork();
}

void InteractiveEngine::dispatch(NewSessionAction) {
    session_flows_->open_new();
}

void InteractiveEngine::dispatch(CopyLastMessageAction) {
    handle_copy_last_message();
}

void InteractiveEngine::dispatch(OpenTreeSelectorAction) {
    session_flows_->open_tree();
}

void InteractiveEngine::dispatch(SuspendAction) {
    if (suspend_controller_) suspend_controller_->suspend();
}

void InteractiveEngine::dispatch(ExternalEditorAction) {
    const auto self = shared_from_this();
    auto bridged = support::detail::make_async_result_on(
            executor_, [self]() mutable -> boost::asio::awaitable<support::ExpectedVoid> {
                co_await self->handle_open_external_editor();
                co_return support::ExpectedVoid{};
            });
    std::move(bridged).start([self](support::ExpectedVoid result) noexcept {
        if (!result) {
            // The bridge maps a launch/body failure to the generic
            // outcome; the strict build treats a failure as a Runtime
            // invariant (the coroutine converts its own failures).
            self->show_error("External editor failed");
        }
    });
}

boost::asio::awaitable<void> InteractiveEngine::handle_open_external_editor() {
    if (view_ == nullptr) co_return;
    auto result = co_await run_external_editor_flow(
        tui_, view_->editor_expanded_text());
    if (!result) {
        completion_result_ = std::unexpected(result.error());
        request_exit();
        co_return;
    }
    if (*result && view_ != nullptr) {
        view_->set_editor_text(std::move(**result));
        tui_.invalidate();
    }
}

bool InteractiveEngine::is_dynamic_slash_command(std::string_view command) const {
    if (session_ == nullptr) return false;
    return tui::is_dynamic_slash_command(
        command,
        session_->templates(),
        session_->skills(),
        settings_manager_ && settings_manager_->get_enable_skill_commands());
}

bool InteractiveEngine::dispatch_command(std::string_view text) {
    if (!running_ || view_ == nullptr) return false;

    SlashCommandExecutionContext context;
    context.execute_immediate = [this](const SlashCommandInvocation& invocation) {
        return execute_immediate_slash_command(invocation);
    };
    context.allow_unrecognized = [this](std::string_view command) {
        // The router removes only the command prefix. Absolute paths such
        // as clipboard image paths inserted into the editor retain an
        // internal slash in the token and are ordinary prompt text, not
        // command tokens.
        return command.find('/') != std::string_view::npos ||
            is_dynamic_slash_command(command);
    };

    auto routed = slash_command_router_.route(text, context);
    if (std::holds_alternative<SlashCommandPassThrough>(routed)) {
        return false;
    }
    if (auto* error = std::get_if<SlashCommandRouteError>(&routed)) {
        show_error(std::move(error->message));
        return true;
    }
    if (auto* modal = std::get_if<SlashCommandModalResult>(&routed)) {
        dispatch_modal_slash_command(std::move(modal->invocation));
    }
    return true;
}

InteractiveEngine::InterruptRoute InteractiveEngine::admit_interrupt(
    std::size_t captured_generation,
    bool pending_bash) noexcept {
    if (captured_generation != prompt_generation_.load()) return InterruptRoute::None;
    if (prompt_active_) {
        if (interrupt_requested_generation_ == prompt_generation_.load()) {
            return InterruptRoute::None;
        }
        interrupt_requested_generation_ = prompt_generation_.load();
        return InterruptRoute::AbortAgentRun;
    }
    if (user_bash_active_) return InterruptRoute::CancelUserBash;
    if (pending_bash) return InterruptRoute::ClearPendingBash;
    return InterruptRoute::None;
}

bool InteractiveEngine::dispatch_user_bash(const std::string& text, SubmissionOrigin origin) {
    if (origin != SubmissionOrigin::FocusedEditor) return false;
    if (!detail::AgentSessionInteractiveAccess::has_user_shell(*session_)) return false;
    auto invocation = parse_user_bash_invocation(text);
    if (!invocation) return false;
    if (user_bash_active_) {
        // pi: "A bash command is already running..." and setText(text).
        view_->restore_submitted_text(trim_editor_submission(text));
        view_->append_user_bash_diagnostic(
            "A User Bash command is already in flight");
        tui_.invalidate();
        return true;
    }

    user_bash_active_ = true;
    // The original trimmed submission is what failure restores to the
    // editor (pi setText(text), ADR 0028) — never a re-serialized form.
    auto recall = trim_editor_submission(text);
    const auto self = shared_from_this();
    const std::size_t started_generation = action_generation_;
    auto bridged = support::detail::make_async_result_on(executor_,
            [self, invocation = std::move(invocation), recall = std::move(recall), started_generation]() mutable
                    -> boost::asio::awaitable<support::ExpectedVoid> {
                auto result = co_await detail::AgentSessionInteractiveAccess::run_user_bash(*self->session_,
                        std::move(invocation->command),
                        invocation->exclude_from_context,
                        [self](const runtime::UserBashProgress& progress) -> support::ExpectedVoid {
                            if (self->running_ && self->view_ != nullptr) {
                                self->view_->set_user_bash_progress(progress);
                                self->tui_.invalidate();
                            }
                            return {};
                        });
                self->user_bash_finished(started_generation, std::move(result), recall);
                co_return support::ExpectedVoid{};
            });
    std::move(bridged).start(
        [weak = weak_from_this(), started_generation](support::ExpectedVoid result) noexcept {
            if (result) return;
            if (const auto self = weak.lock()) {
                if (self->generation_retired(started_generation)) {
                    // A launch failure from a retired Session generation
                    // cannot mutate or render as the current Session
                    // (issue #466).
                    return;
                }
                self->user_bash_active_ = false;
                if (self->view_ != nullptr && self->running_) {
                    self->view_->clear_user_bash_progress();
                    self->view_->append_user_bash_diagnostic(
                        "Native TUI User Bash coroutine failed");
                    self->tui_.invalidate();
                }
                if (self->exit_requested_) self->signal_exit();
            }
        });
    return true;
}

void InteractiveEngine::request_interrupt(
    std::size_t prompt_generation,
    const EditorInterruptRequest& request) {
    if (!running_ || exit_requested_) return;
    switch (admit_interrupt(prompt_generation, request.pending_bash)) {
    case InterruptRoute::AbortAgentRun:
        // pi restores queued input before aborting the Agent run.
        dequeue_pending_input(false);
        session_->abort();
        return;
    case InterruptRoute::CancelUserBash:
        detail::AgentSessionInteractiveAccess::cancel_user_bash(*session_);
        return;
    case InterruptRoute::ClearPendingBash:
        cleared_editor_revision_ = request.editor_revision;
        if (view_ != nullptr) {
            view_->clear_pending_bash(request);
            tui_.invalidate();
        }
        return;
    case InterruptRoute::None:
        // pi's `onEscape` tail: an idle editor with no text runs the
        // double-escape window (`doubleEscapeAction` default "tree", 500
        // ms); the settings field stays out of the subset, so the tree
        // trigger is hard-coded exactly like pi's default.
        if (trim_editor_submission(request.pending_bash_text).empty()) {
            const auto now = std::chrono::steady_clock::now();
            if (now - last_escape_time_ < std::chrono::milliseconds{500}) {
                last_escape_time_ = {};
                session_flows_->open_tree();
            } else {
                last_escape_time_ = now;
            }
        }
        return;
    }
}

void InteractiveEngine::submit(
    std::string text,
    InputSubmission submission,
    PromptOptions options,
    SubmissionOrigin origin,
    std::optional<std::size_t> editor_revision) {
    if (!running_ || view_ == nullptr || text.empty()) return;
    if (origin == SubmissionOrigin::FocusedEditor && editor_revision &&
        cleared_editor_revision_ == editor_revision) {
        return;
    }
    // pi handleFollowUp: while a run is active, Alt+Enter queues the
    // trimmed text directly as follow-up input — the editor chain (User
    // Bash parse, slash dispatch) does not run, and prompt-template
    // expansion happens inside the session admission; when idle,
    // Alt+Enter acts like regular Enter and runs the full editor chain.
    const bool follow_up_while_active =
        submission == InputSubmission::FollowUp && prompt_active_;
    if (!follow_up_while_active) {
        if (dispatch_user_bash(text, origin)) return;
        if (dispatch_command(text)) return;
    }

    if (prompt_active_) {
        if (interrupt_requested()) {
            // The active run was already asked to abort; queued input
            // would die with it, so the text returns to the editor.
            view_->restore_submitted_text(text);
            view_->append_diagnostic("A prompt is already in flight");
            tui_.invalidate();
            return;
        }
        if (submission == InputSubmission::FollowUp) {
            if (auto admitted = session_->follow_up(text); !admitted) {
                view_->restore_submitted_text(text);
                view_->append_diagnostic(bounded_redacted_presentation(std::format(
                    "Unable to queue follow-up input: {}",
                    combined_error_text(admitted.error()))));
            }
        } else {
            if (auto admitted = session_->steer(text); !admitted) {
                view_->restore_submitted_text(text);
                view_->append_diagnostic(bounded_redacted_presentation(std::format(
                    "Unable to queue steering input: {}",
                    combined_error_text(admitted.error()))));
            }
        }
        session_ui_->sync_pending_input();
        tui_.invalidate();
        return;
    }

    note_prompt_started();
    prompt_active_ = true;
    const auto self = shared_from_this();
    const std::size_t started_generation = action_generation_;
    auto bridged = support::detail::make_async_result_on(executor_,
            [self, text = std::move(text), options = std::move(options), started_generation]() mutable
                    -> boost::asio::awaitable<support::ExpectedVoid> {
                support::ExpectedVoid result;
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
                try {
#endif
                    result = co_await self->session_->prompt(text, std::move(options));
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
                } catch (const std::exception& error) {
                    result = std::unexpected(
                            support::make_error(support::ErrorCode::Unknown, "Native TUI prompt failed", error.what()));
                } catch (...) {
                    result = std::unexpected(support::make_error(
                            support::ErrorCode::Unknown, "Native TUI prompt failed", "unknown exception"));
                }
#endif
                self->prompt_finished(started_generation, std::move(result), text);
                co_return support::ExpectedVoid{};
            });
    std::move(bridged).start(
        [weak = weak_from_this(), started_generation](support::ExpectedVoid result) noexcept {
            if (result) return;
            if (const auto self = weak.lock()) {
                self->prompt_launch_failed(started_generation);
            }
        });
}

void InteractiveEngine::prompt_launch_failed(std::size_t started_generation) {
    if (generation_retired(started_generation)) {
        // A launch failure from a retired Session generation cannot
        // mutate or render as the current Session (issue #466).
        return;
    }
    note_prompt_finished();
    prompt_active_ = false;
    if (view_ != nullptr && running_) {
        view_->append_diagnostic("Native TUI prompt failed");
        tui_.invalidate();
    }
    if (exit_requested_) signal_exit();
}

void InteractiveEngine::prompt_finished(
    std::size_t started_generation,
    support::ExpectedVoid result,
    const std::string& submitted_text) {
    if (generation_retired(started_generation)) {
        // A completion from a retired Session generation (the Session was
        // replaced or closed): the old Session's close already retired its
        // interrupt generation and cleared the active-work facts, so this
        // late completion must not render, mutate, or un-gate the current
        // Session (issue #466).
        return;
    }
    note_prompt_finished();
    prompt_active_ = false;
    session_ui_->sync_session_observations();
    if (!result && view_ != nullptr && running_) {
        view_->append_diagnostic(combined_error_text(result.error()));
        view_->restore_submitted_text(submitted_text);
        tui_.invalidate();
    }
    if (exit_requested_ && !user_bash_active_) signal_exit();
}

void InteractiveEngine::user_bash_finished(
    std::size_t started_generation,
    support::Expected<runtime::UserBashCompletion> result,
    const std::string& recall) {
    if (generation_retired(started_generation)) {
        // A completion from a retired Session generation (the Session was
        // replaced or closed): the old Session's close already retired its
        // active-work facts, so this late completion must not commit or
        // render as the current Session (issue #466).
        return;
    }
    user_bash_active_ = false;
    if (view_ != nullptr && running_) {
        if (result) {
            view_->commit_user_bash(
                ai::MessageVariant{std::move(result->message)});
            if (result->diagnostic) {
                view_->append_user_bash_diagnostic(
                    combined_error_text(*result->diagnostic));
            }
        } else {
            view_->clear_user_bash_progress();
            view_->append_user_bash_diagnostic(combined_error_text(result.error()));
            if (!recall.empty()) {
                view_->restore_submitted_text(recall);
            }
        }
        tui_.invalidate();
    }
    if (exit_requested_ && !prompt_active_) signal_exit();
}

boost::asio::awaitable<support::ExpectedVoid> run_interactive_mode(
    cch::tui::Terminal& terminal,
    InteractiveSessionRun run) {
    const auto executor = co_await boost::asio::this_coro::executor;
    const bool is_boot = std::holds_alternative<DeferBoot>(run.session_intent());
    auto engine = std::make_shared<InteractiveEngine>(terminal, executor);
    if (auto started = engine->start(std::move(run)); !started) {
        co_return std::unexpected(started.error());
    }
    if (is_boot) {
        if (auto booted = co_await engine->boot_session(); !booted) {
            // The boot-created session failed before bind; `boot_session`
            // already stopped the TUI (the creation-failure sink printed pi's
            // message).
            co_return std::unexpected(booted.error());
        }
    }

    boost::system::error_code wait_error;
    co_await engine->exit_wait().async_wait(
        boost::asio::redirect_error(boost::asio::use_awaitable, wait_error));
    co_return co_await engine->finish();
}

} // namespace cch::coding_agent::tui

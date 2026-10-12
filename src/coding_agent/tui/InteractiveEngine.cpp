// InteractiveEngine composition and lifecycle unit (#506): startup resource
// loading, view composition, the ModalPresenter seam, overlay management,
// executor hops, and exit gating. See InteractiveEngine.hpp for the unit map.

#include "InteractiveEngine.hpp"

#include "coding_agent/runtime/AgentSessionInteractiveAccess.hpp"
#include "coding_agent/tui/AuthFlowController.hpp"
#include "coding_agent/tui/EditorAutocomplete.hpp"
#include "coding_agent/tui/ErrorPresentation.hpp"
#include "coding_agent/tui/ImageTranscoder.hpp"
#include "coding_agent/tui/InteractiveSessionRun.hpp"
#include "coding_agent/tui/InteractiveView.hpp"
#include "coding_agent/tui/LoadedResources.hpp"
#include "coding_agent/tui/McpManagerFlow.hpp"
#include "coding_agent/tui/ModelFlowController.hpp"
#include "coding_agent/tui/SessionFlowController.hpp"
#include "coding_agent/tui/SessionUiBinding.hpp"
#include "coding_agent/tui/SharedKeybindings.hpp"
#include "coding_agent/tui/ThemeController.hpp"
#include "support/AsyncResultBridge.hpp"

#include <cch/coding_agent/AgentConfigDir.hpp>
#include <cch/coding_agent/ProjectTrust.hpp>
#include <cch/tui/Overlay.hpp>
#include <cch/tui/Terminal.hpp>

#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace cch::coding_agent::tui {
namespace {

/// pi `renderProjectTrustWarningIfNeeded` chat warning text, with the C++
/// binary's own identity and the absent packages clause dropped.
[[nodiscard]] std::string project_trust_warning_text() {
    return "This project is not trusted. Project .pi resources are ignored. "
           "Use /trust to save a trust decision, then restart pike.";
}

[[nodiscard]] support::Error project_resource_detection_unavailable_error() {
    return support::make_error(
            support::ErrorCode::Workspace, "Project resource detection filesystem capability is unavailable");
}

class AsioTuiScheduler final : public cch::tui::TuiScheduler {
public:
    explicit AsioTuiScheduler(boost::asio::any_io_executor executor)
        : executor_(std::move(executor)), timer_(executor_) {}

    [[nodiscard]] std::chrono::steady_clock::time_point now() const noexcept override {
        return std::chrono::steady_clock::now();
    }

    void post(cch::tui::TuiScheduledWork work) override {
        boost::asio::post(executor_, [work = std::move(work)]() mutable {
            if (work) work();
        });
    }

    void schedule(std::chrono::steady_clock::duration delay, cch::tui::TuiScheduledWork work) override {
        timer_.expires_after(std::max(delay, std::chrono::steady_clock::duration::zero()));
        timer_.async_wait([work = std::move(work)](const boost::system::error_code& error) mutable {
            if (!error && work) work();
        });
    }

    void cancel() noexcept override { (void)timer_.cancel(); }

private:
    boost::asio::any_io_executor executor_;
    boost::asio::steady_timer timer_;
};

} // namespace

using interactive_view_detail::queued_editor_texts;

InteractiveEngine::~InteractiveEngine() = default;

InteractiveEngine::InteractiveEngine(cch::tui::Terminal& terminal, boost::asio::any_io_executor executor)
    : session_(nullptr), terminal_(terminal), tui_scheduler_(std::make_unique<AsioTuiScheduler>(executor)),
      main_screen_(terminal), tui_(main_screen_), executor_(std::move(executor)), exit_wait_(executor_),
      flows_settled_(executor_) {
    exit_wait_.expires_at(std::chrono::steady_clock::time_point::max());
    flows_settled_.expires_at(std::chrono::steady_clock::time_point::max());
    tui_.set_scheduler(tui_scheduler_.get());
}

support::ExpectedVoid InteractiveEngine::start(InteractiveSessionRun run) {
    auto intent = run.take_session_intent();
    std::visit(
            [this](auto&& it) {
                using T = std::decay_t<decltype(it)>;
                if constexpr (std::is_same_v<T, BindExistingSession>) {
                    session_ = it.session;
                    boot_request_ = std::nullopt;
                } else if constexpr (std::is_same_v<T, DeferBoot>) {
                    session_ = nullptr;
                    boot_request_ = std::move(it.request);
                }
            },
            intent);
    const bool booting = boot_request_.has_value();

    runtime_root_ = run.runtime_root();
    project_resource_filesystems_ = run.take_project_resource_filesystems();
    if (!project_resource_filesystems_.workspace && runtime_root_) {
        const auto workspace = session_ != nullptr
                                       ? session_->workspace()
                                       : (boot_request_ ? boot_request_->workspace : std::filesystem::path{});
        if (!workspace.empty()) {
            project_resource_filesystems_ = make_project_resource_filesystems(
                    runtime_root_, workspace, run.agent_config_directory(), coding_agent::home_directory());
        }
    }

    native_clipboard_ = run.take_native_clipboard();
    stop_source_ = std::stop_source{};
    model_fallback_message_ = run.model_fallback_message();
    action_sink_ = run.make_action_sink();
    async_session_replacement_sink_ = run.make_async_session_replacement_sink();
    session_facts_ = run.session_facts();

    InteractiveStartupDiagnostics diagnostics;
    if (auto loaded = load_startup_resources(run); !loaded) {
        return fail_start(loaded.error());
    } else {
        diagnostics = std::move(*loaded);
    }

    // The model flows need the startup resources (live theme, settings,
    // keybindings), so the controller is created right after they load;
    // the initial `/model` completion snapshot follows (nothing reads it
    // before view composition).
    model_flows_ = make_model_flow_controller();
    auth_flows_ = make_auth_flow_controller();
    mcp_flows_ = make_mcp_flow_controller();
    session_flows_ = make_session_flow_controller();
    session_ui_ = make_session_ui_binding();
    settings_flows_ = make_settings_flow_controller();
    suspend_controller_ = make_suspend_controller();
    if (!booting) {
        model_flows_->update_model_completion();
    }

    const auto weak = weak_from_this();
    auto view = make_interactive_view(weak);
    view_ = view.get();
    if (auto attached = tui_.add_child(std::move(view)); !attached) {
        return fail_start(attached.error());
    }
    if (!booting) {
        if (session_ == nullptr) {
            return fail_start(support::make_error(
                    support::ErrorCode::Unknown, "BindExistingSession intent supplied a null session"));
        }
        if (auto subscribed = session_ui_->bind(*session_); !subscribed) {
            return fail_start(subscribed.error());
        }
    }

    tui_.set_render_prepare_sink([weak]() -> support::Expected<bool> {
        if (const auto self = weak.lock()) return self->prepare_frame();
        return false;
    });
    tui_.set_render_complete_sink([weak](const support::ExpectedVoid& result) {
        if (const auto self = weak.lock()) self->complete_frame(result);
    });
    if (auto started = tui_.start(); !started) return fail_start(started.error());
    tui_started_ = true;
    running_ = true;

    // pi main.ts `ensurePngTranscoder` right after `ui.start()`: a Kitty
    // terminal only places non-PNG images once the host conversion capability
    // is registered, and the hook repaints images that fell back to text
    // because a registration landed after their first frame (issue #996).
    ensure_png_transcoder([weak]() {
        if (const auto self = weak.lock()) self->invalidate_frame();
    });

    if (!booting) {
        initialize_view(diagnostics);
        // The `/mcp` refresh drains the manager's latched warnings into the
        // chat, so on a bind-existing start it must follow view creation —
        // running it with the controller block above loses the warnings to
        // the still-null view. The editor provider built with the view saw
        // an empty MCP snapshot; rebuild it from the fresh one (the boot
        // bind at `bind_boot_session` already orders it this way).
        mcp_flows_->refresh_completion();
        rebuild_autocomplete_provider();
        if (auto focused = tui_.set_focus(view_); !focused) return fail_start(focused.error());
        prompt_presentation_dirty_.store(true, std::memory_order_release);
        tui_.request_render();
        if (run.initial_prompt()) {
            submit(*run.initial_prompt(),
                    InputSubmission::Ordinary,
                    run.initial_prompt_options(),
                    SubmissionOrigin::InitialPrompt);
        }
    } else {
        // pi main.ts: the boot trust prompt resolves as an overlay on the
        // main TUI before session bind (G2 record); `boot_session`
        // creates the boot session and then binds the view. The startup
        // diagnostics render after bind, alongside the created session's
        // snapshot.
        startup_diagnostics_ = std::move(diagnostics);
        if (auto focused = tui_.set_focus(view_); !focused) return fail_start(focused.error());
        prompt_presentation_dirty_.store(true, std::memory_order_release);
        tui_.request_render();
        initial_prompt_ = run.initial_prompt();
        initial_prompt_options_ = run.initial_prompt_options();
    }
    return {};
}

boost::asio::awaitable<support::ExpectedVoid> InteractiveEngine::boot_session() {
    // 1. Resolve boot trust (pi resolveProjectTrusted): override → no
    //    trust-requiring resources → saved decision → default
    //    always/never → ask prompt (the generic string-list selector
    //    overlay; G2 record). The controller also arms pi's implicit
    //    trust-on-reload behavior for a resource-free boot.
    if (auto armed = co_await session_flows_->arm_auto_trust_on_reload(
                boot_request_->workspace, boot_request_->project_trust_override);
            !armed) {
        (void)deliver_action(action_generation_, TuiActionVariant{ReportBootCreationFailureAction{armed.error()}});
        auto failed = fail_start(armed.error());
        co_return std::unexpected(failed.error());
    }
    auto decision = co_await session_flows_->resolve_boot_trust(
            boot_request_->workspace, boot_request_->project_trust_override);
    if (!decision) {
        (void)deliver_action(action_generation_, TuiActionVariant{ReportBootCreationFailureAction{decision.error()}});
        auto failed = fail_start(decision.error());
        co_return std::unexpected(failed.error());
    }
    // pi `projectTrustByCwd`: remember the boot decision for the boot
    // workspace so in-session session creations in the same workspace
    // reuse it instead of re-resolving (ask-without-UI would silently
    // drop a session-only trust).
    resolved_boot_trust_.emplace(boot_request_->workspace, *decision);
    // 2. Create the boot session with the decided trust so SessionFactory
    //    resolves deterministically (pi `projectTrustByCwd` cache).
    auto request = std::move(*boot_request_);
    request.project_trust_override = *decision;
    auto created = co_await request_session_replacement_async(action_generation_, std::move(request));
    if (!created) {
        const support::Error failure = created.error();
        // pi `print_creation_failure`: the host reports the failure
        // through the closed action seam before the boot exits.
        (void)deliver_action(action_generation_, TuiActionVariant{ReportBootCreationFailureAction{failure}});
        // Stop the TUI so the terminal is restored before the host
        // reports the error; the session never bound.
        running_ = false;
        if (tui_started_) {
            (void)tui_.stop();
            tui_started_ = false;
        }
        co_return std::unexpected(failure);
    }
    // pi `reportDiagnostics`: keep the action-seam notification and retain a
    // copy for the chat view; the action marks that the TUI owns presentation
    // so the host does not corrupt its terminal by writing to stderr.
    if (!created->diagnostics.empty()) {
        auto diagnostics_for_view = created->diagnostics;
        (void)deliver_action(action_generation_,
                TuiActionVariant{ReportBootDiagnosticsAction{
                        .diagnostics = std::move(created->diagnostics),
                        .rendered_in_tui = true,
                }});
        startup_diagnostics_.session.insert(startup_diagnostics_.session.end(),
                std::make_move_iterator(diagnostics_for_view.begin()),
                std::make_move_iterator(diagnostics_for_view.end()));
    }
    model_fallback_message_ = std::move(created->model_fallback_message);
    // pi interactive-mode ctor `setRegisteredThemes(...)` + init
    // `applyFromSettings()`: register the boot session's discovered
    // themes (project `.pi/themes` trust-gated, user directory,
    // explicit `--theme`) and re-apply the active theme from the
    // settings with dark fallback (pi `/reload` re-runs the same two
    // steps). Parse/collision diagnostics render with the startup
    // diagnostics.
    if (theme_controller_) {
        auto discovery = coding_agent::tui::discover_themes(std::move(created->theme_resources));
        loaded_theme_diagnostics_ = discovery.diagnostics;
        startup_diagnostics_.themes = std::move(discovery.diagnostics);
        theme_controller_->set_registered_themes(std::move(discovery.themes));
        theme_controller_->apply_from_settings();
    }
    // 3. Bind: the boot-created session replaces the borrowed null
    //    session and the presentation renders its snapshot like pi's
    //    `renderInitialMessages`.
    owned_session_ = std::move(created->session);
    session_ = owned_session_.get();
    model_flows_->update_model_completion();
    mcp_flows_->refresh_completion();
    rebuild_autocomplete_provider();
    if (auto subscribed = session_ui_->bind(*session_); !subscribed) {
        co_return std::unexpected(subscribed.error());
    }
    initialize_view(startup_diagnostics_);
    if (auto warning = co_await append_project_trust_warning_if_needed(); !warning) {
        auto failed = fail_start(warning.error());
        co_return std::unexpected(failed.error());
    }
    if (auto focused = tui_.set_focus(view_); !focused) {
        co_return std::unexpected(focused.error());
    }
    prompt_presentation_dirty_.store(true, std::memory_order_release);
    tui_.request_render();
    if (initial_prompt_) {
        submit(std::move(*initial_prompt_),
                InputSubmission::Ordinary,
                std::move(initial_prompt_options_),
                SubmissionOrigin::InitialPrompt);
    }
    co_return support::ExpectedVoid{};
}

boost::asio::awaitable<support::ExpectedVoid> InteractiveEngine::finish() {
    stop_source_.request_stop();
    tui_.set_render_prepare_sink(nullptr);
    tui_.set_render_complete_sink(nullptr);
    // Cancel extracted modal/session flows before terminal restoration;
    // their host-lifetime captures then let their coroutines quiesce
    // without touching a stopped presenter.
    if (auth_flows_) auth_flows_->close();
    if (session_flows_) session_flows_->close();
    running_ = false;
    // Retire the action generation so late deliveries from captured
    // hooks are rejected after Close (ADR 0040).
    retire_current_session();
    // Await every admitted controller flow to quiesce (ADR 0040: no
    // resource is destroyed while an admitted operation can still use
    // it). The counter is executor-confined; the last flow to finish
    // cancels the timer and releases this wait.
    if (in_flight_flows_ > 0) {
        flows_settled_.expires_at(std::chrono::steady_clock::time_point::max());
        boost::system::error_code wait_error;
        co_await flows_settled_.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, wait_error));
    }
    const auto stopped = tui_.stop();
    tui_.set_scheduler(nullptr);
    tui_started_ = false;
    if (!completion_result_) completion_result_.emplace();
    if (!*completion_result_) {
        if (!stopped) {
            co_return std::unexpected(aggregate_presentation_errors(completion_result_->error(),
                    stopped.error(),
                    first_frame_rendered_ ? "Native TUI failed and terminal restoration failed"
                                          : "Native TUI startup and terminal restoration failed"));
        }
        co_return std::unexpected(completion_result_->error());
    }
    if (!stopped) {
        co_return std::unexpected(presentation_error(stopped.error(), "Native TUI terminal restoration failed"));
    }
    co_return support::ExpectedVoid{};
}

support::ExpectedVoid InteractiveEngine::re_catalog_keybindings() {
    const auto actions = assemble_keybinding_actions(native_clipboard_ != nullptr);
    if (auto manager = load_app_keybinding_manager(agent_config_directory_, actions); !manager) {
        return std::unexpected(manager.error());
    } else {
        if (view_ != nullptr) {
            view_->set_keybindings(manager->registry, manager->help);
            for (const auto& diagnostic : manager->diagnostics) {
                view_->append_diagnostic(diagnostic.message);
            }
            invalidate_frame();
        }
    }
    return {};
}

support::Expected<InteractiveStartupDiagnostics> InteractiveEngine::load_startup_resources(
        const InteractiveSessionRun& run) {
    InteractiveStartupDiagnostics diagnostics;
    agent_config_directory_ = run.agent_config_directory();
    const auto actions = assemble_keybinding_actions(native_clipboard_ != nullptr);
    if (auto manager = load_app_keybinding_manager(run.agent_config_directory(), actions); !manager) {
        return std::unexpected(manager.error());
    } else {
        // The shared slot exists before the view is composed; the
        // `/reload` re-catalog replaces through the same slot (ADR
        // 0035, #418).
        if (!keybindings_) {
            keybindings_ = std::make_shared<SharedKeybindings>();
        }
        keybindings_->replace(manager->registry, manager->help);
        diagnostics.keybindings = std::move(manager->diagnostics);
    }

    // The manager stays owned by the state: the scoped-models selector
    // persists `enabledModels` through it (pi `setEnabledModels`) and the
    // theme committer below references it.
    if (auto manager = create_interactive_settings_manager(run.agent_config_directory()); !manager) {
        return std::unexpected(manager.error());
    } else {
        settings_manager_.emplace(std::move(*manager));
    }
    // pi `init()`: the render settings load once at boot
    // (`settingsManager.getHideThinkingBlock()` / `getOutputPad()`) and
    // apply to the chat; changes persist through the settings manager
    // and re-apply live (the two G2-graduated fields).
    hide_thinking_block_ = settings_manager_->hide_thinking_block();
    output_pad_ = settings_manager_->output_pad();
    // pi interactive-mode ctor: the theme controller boots against the
    // env-only COLORFGBG terminal theme; registered themes arrive with
    // the boot session (the C++ boot defers session creation until after
    // the boot trust prompt, so registration happens at bind and
    // `applyFromSettings` re-applies afterwards).
    const auto weak_controller = weak_from_this();
    InteractiveThemeHooks theme_hooks;
    theme_hooks.show_error = [weak_controller](std::string message) {
        if (const auto self = weak_controller.lock()) {
            self->post_from_view(
                    [message = std::move(message)](InteractiveEngine& state) { state.show_error(std::move(message)); });
        }
    };
    theme_hooks.on_changed = [weak_controller] {
        // pi `onChanged` → `updateEditorBorderColor`: the C++ editor
        // border re-derives from the live palette at render, so the
        // change notification requests a render (pi's
        // `ui.requestRender`); the controller itself invalidates.
        if (const auto self = weak_controller.lock()) {
            self->post_invalidate();
        }
    };
    theme_controller_ = make_interactive_theme_controller(run.agent_config_directory(),
            *settings_manager_,
            terminal_.capabilities().color,
            tui_,
            std::move(theme_hooks));
    return diagnostics;
}

std::unique_ptr<InteractiveView> InteractiveEngine::make_interactive_view(std::weak_ptr<InteractiveEngine> weak) {
    InteractiveViewOptions options;
    options.keybindings = keybindings_;
    // Preserve the existing production hint: the application supplies
    // the clipboard action path even when the clipboard reader is
    // unavailable, so the hint remains part of the assembled Native TUI.
    options.clipboard_paste_available = true;
    // Render invalidation stays a separate coalescible request (not part
    // of the action seam).
    options.on_invalidate = [weak] {
        if (const auto self = weak.lock()) self->post_invalidate();
    };
    // One closed action seam (ADR 0040 shape): every main-screen action
    // is admitted through post_view_action, which captures the interrupt
    // prompt generation at admission and posts exactly once to the
    // serialized executor path.
    options.action_sink = [weak](ViewAction action) noexcept -> support::ExpectedVoid {
        if (const auto self = weak.lock()) {
            self->post_view_action(std::move(action));
        }
        return support::ExpectedVoid{};
    };
    options.footer_data_source = [weak] {
        if (const auto self = weak.lock(); self && self->running_) {
            return self->session_ui_->compute_footer_data();
        }
        return FooterData{};
    };
    options.hide_thinking_block = hide_thinking_block_;
    options.output_pad = output_pad_;
    options.user_bash_available = view_user_shell_available();
    options.autocomplete_provider = build_autocomplete_provider();
    options.autocomplete_debounce_timer = std::make_unique<AsioAutocompleteDebounceTimer>(executor_);
    options.render_request = [weak]() -> support::ExpectedVoid {
        if (const auto self = weak.lock()) self->post_invalidate();
        return {};
    };
    options.theme = &theme_controller_->live_theme();
    return std::make_unique<InteractiveView>(std::move(options));
}

bool InteractiveEngine::view_user_shell_available() const {
    if (session_ != nullptr) {
        return detail::AgentSessionInteractiveAccess::has_user_shell(*session_);
    }
    return boot_request_ && boot_request_->provide_user_shell;
}

std::unique_ptr<cch::tui::AutocompleteProvider> InteractiveEngine::build_autocomplete_provider() {
    const bool include_skill_commands = settings_manager_ && settings_manager_->get_enable_skill_commands();
    static const std::vector<PromptTemplate> kEmptyTemplates;
    static const std::vector<Skill> kEmptySkills;
    const auto& templates = session_ != nullptr ? session_->templates() : kEmptyTemplates;
    const auto& skills = session_ != nullptr ? session_->skills() : kEmptySkills;
    const auto workspace = session_ != nullptr ? session_->workspace()
                                               : (boot_request_ ? boot_request_->workspace : std::filesystem::path{});
    // Every result must reach the editor's serialized domain on this
    // executor: the provider contract allows worker-thread delivery, and the
    // receiver re-enters the editor through the render request (#609).
    return std::make_unique<ExecutorAutocompleteProvider>(executor_,
            build_editor_autocomplete_provider(templates,
                    skills,
                    model_flows_->model_completion(),
                    mcp_flows_ ? mcp_flows_->completion_snapshot() : nullptr,
                    include_skill_commands,
                    workspace));
}

void InteractiveEngine::rebuild_autocomplete_provider() {
    if (view_ == nullptr) {
        return;
    }
    view_->set_autocomplete_provider(build_autocomplete_provider());
}

void InteractiveEngine::refresh_loaded_resources() {
    if (view_ == nullptr || session_ == nullptr) {
        return;
    }
    static const std::vector<RegisteredTheme> kNoRegisteredThemes;
    const auto& registered = theme_controller_ ? theme_controller_->registered_themes() : kNoRegisteredThemes;
    view_->set_loaded_resources_data(collect_loaded_resources_data(*session_, registered, loaded_theme_diagnostics_));
}

void InteractiveEngine::initialize_view(const InteractiveStartupDiagnostics& diagnostics) {
    const auto snapshot = session_->snapshot();
    view_->initialize(snapshot);
    view_->set_pending_input(snapshot.agent_state.input_queues);
    refresh_loaded_resources();
    // pi `interactive-mode.ts` `init()`: the model fallback message shows
    // as a boot warning line (`showWarning`) before the initial prompt.
    if (model_fallback_message_) {
        view_->append_warning(*model_fallback_message_);
    }
    session_ui_->append_snapshot_diagnostics(snapshot.agent_state.diagnostics);
    std::vector<std::string> rendered_diagnostics;
    const auto append_once = [&rendered_diagnostics](std::string text, auto&& append) {
        if (std::ranges::find(rendered_diagnostics, text) != rendered_diagnostics.end()) return;
        rendered_diagnostics.push_back(text);
        append(std::move(text));
    };
    for (const auto& diagnostic : diagnostics.session) {
        auto text = diagnostic.message;
        if (diagnostic.path) text += " (" + *diagnostic.path + ')';
        switch (diagnostic.severity) {
        case SessionDiagnostic::Severity::Info:
            append_once(std::move(text), [this](std::string value) { view_->append_status_message(std::move(value)); });
            break;
        case SessionDiagnostic::Severity::Warning:
            append_once(std::move(text), [this](std::string value) { view_->append_warning(std::move(value)); });
            break;
        case SessionDiagnostic::Severity::Error:
            append_once(std::move(text), [this](std::string value) { view_->append_diagnostic(std::move(value)); });
            break;
        }
    }
    for (const auto& diagnostic : diagnostics.keybindings) {
        append_once(diagnostic.message, [this](std::string value) { view_->append_warning(std::move(value)); });
    }
    for (const auto& diagnostic : diagnostics.themes) {
        append_once(diagnostic.message, [this](std::string value) { view_->append_warning(std::move(value)); });
    }
}

ProjectResourceFileSystems InteractiveEngine::project_resource_filesystems_for(const std::filesystem::path& workspace) {
    const auto requested = workspace.lexically_normal();
    if (project_resource_filesystems_.workspace &&
            project_resource_filesystems_.workspace->workspace().lexically_normal() == requested) {
        return project_resource_filesystems_;
    }
    if (!runtime_root_) {
        return {};
    }
    return make_project_resource_filesystems(
            runtime_root_, workspace, agent_config_directory_, coding_agent::home_directory());
}

boost::asio::awaitable<support::Expected<bool>> InteractiveEngine::append_project_trust_warning_if_needed() {
    auto* session = session_;
    if (session == nullptr || detail::AgentSessionInteractiveAccess::is_project_trusted(*session)) {
        co_return false;
    }
    const auto captured_generation = action_generation_;
    const auto workspace = session->workspace();
    auto filesystems = project_resource_filesystems_for(workspace);
    if (!filesystems.workspace) {
        co_return std::unexpected(project_resource_detection_unavailable_error());
    }
    auto detection = co_await support::detail::await_async_result(coding_agent::detect_project_resources(
            std::move(filesystems), coding_agent::home_directory() / ".agents" / "skills", stop_source_.get_token()));
    if (!running_ || stop_source_.stop_requested() || captured_generation != action_generation_ ||
            session_ != session || session->workspace() != workspace ||
            detail::AgentSessionInteractiveAccess::is_project_trusted(*session)) {
        co_return false;
    }
    if (!detection) {
        co_return std::unexpected(harness::to_util_error(std::move(detection.error())));
    }
    if (!needs_project_trust_resolution(*detection)) {
        co_return false;
    }
    view_->append_trust_warning(project_trust_warning_text());
    request_render();
    co_return true;
}

support::ExpectedVoid InteractiveEngine::fail_start(const support::Error& error) {
    running_ = false;
    if (session_ != nullptr) {
        session_->close();
    }
    support::ExpectedVoid stopped;
    if (tui_started_) stopped = tui_.stop();
    tui_started_ = false;
    if (!stopped) {
        return std::unexpected(aggregate_presentation_errors(
                error, stopped.error(), "Native TUI startup and terminal restoration failed"));
    }
    return std::unexpected(startup_error(error));
}

void InteractiveEngine::post_invalidate() {
    if (!running_) return;
    prompt_presentation_dirty_.store(true, std::memory_order_release);
    const auto weak = weak_from_this();
    boost::asio::post(executor_, [weak] {
        if (const auto self = weak.lock(); self && self->running_) {
            self->prompt_presentation_dirty_.store(true, std::memory_order_release);
            self->tui_.invalidate();
        }
    });
}

void InteractiveEngine::invalidate_frame() {
    prompt_presentation_dirty_.store(true, std::memory_order_release);
    tui_.invalidate();
}

void InteractiveEngine::post_exit() {
    const auto weak = weak_from_this();
    boost::asio::post(executor_, [weak] {
        if (const auto self = weak.lock()) self->request_exit();
    });
}

void InteractiveEngine::post_close_overlay() {
    const auto weak = weak_from_this();
    boost::asio::post(executor_, [weak] {
        if (const auto self = weak.lock()) self->close_overlay();
    });
}

void InteractiveEngine::append_command_error(const support::Error& error) {
    if (view_ == nullptr) return;
    view_->append_diagnostic(combined_error_text(error));
    invalidate_frame();
}

support::ExpectedVoid InteractiveEngine::attach_overlay(std::unique_ptr<cch::tui::Overlay> overlay) {
    auto* overlay_pointer = overlay.get();
    auto attached = tui_.show_overlay(std::move(overlay));
    if (!attached) return std::unexpected(attached.error());
    active_overlay_ = overlay_pointer;
    active_overlay_handle_ = std::move(*attached);
    invalidate_frame();
    return {};
}

void InteractiveEngine::close_overlay() {
    if (!running_ || active_overlay_ == nullptr) return;
    active_overlay_handle_.hide();
    active_overlay_handle_ = {};
    active_overlay_ = nullptr;
    invalidate_frame();
}

void InteractiveEngine::rebuild_chat() {
    if (view_ == nullptr) return;
    view_->apply_render_settings(hide_thinking_block_, output_pad_);
    const auto snapshot = session_->snapshot();
    view_->initialize(snapshot);
    view_->set_pending_input(snapshot.agent_state.input_queues);
    invalidate_frame();
}

/// Post one view-thread action to the executor.
void InteractiveEngine::post_from_view(std::move_only_function<void(InteractiveEngine&)> action) {
    const auto weak = weak_from_this();
    boost::asio::post(executor_, [weak, action = std::move(action)]() mutable {
        if (const auto self = weak.lock(); self && self->running_) action(*self);
    });
}

void InteractiveEngine::show_overlay(std::unique_ptr<cch::tui::Overlay> overlay) {
    if (auto attached = attach_overlay(std::move(overlay)); !attached) {
        append_command_error(attached.error());
    }
}

void InteractiveEngine::replace_prompt_slot(std::shared_ptr<cch::tui::Component> component) {
    if (view_ == nullptr) return;
    view_->set_editor_replacement(std::move(component));
    invalidate_frame();
}

void InteractiveEngine::restore_prompt_slot() {
    if (view_ == nullptr) return;
    view_->restore_editor();
    invalidate_frame();
}

void InteractiveEngine::show_status(std::string text) {
    if (view_ == nullptr) return;
    view_->append_status_message(std::move(text));
    invalidate_frame();
}

void InteractiveEngine::show_error(std::string text) {
    if (view_ == nullptr) return;
    view_->append_diagnostic(std::move(text));
    invalidate_frame();
}

void InteractiveEngine::show_warning(std::string text) {
    if (view_ == nullptr) return;
    view_->append_warning(std::move(text));
    invalidate_frame();
}

void InteractiveEngine::request_render() { invalidate_frame(); }

void InteractiveEngine::invalidate() { invalidate_frame(); }

support::Expected<bool> InteractiveEngine::prepare_frame() {
    if (!running_) return false;
    prepared_counted_frame_ = false;
    if (session_ui_ == nullptr || view_ == nullptr) return true;

    (void)session_ui_->drain_composed_snapshot();
    if (!session_ui_->pending_counted_frame()) return true;
    if (!session_ui_->reconcile_snapshot(session_ui_->composed_snapshot())) return false;
    prepared_counted_frame_ = true;
    return true;
}

void InteractiveEngine::complete_frame(const support::ExpectedVoid& result) {
    if (!result) {
        if (result.error().code == support::ErrorCode::Busy) {
            prompt_presentation_dirty_.store(true, std::memory_order_release);
            return;
        }
        completion_result_ = std::unexpected(presentation_error(
                result.error(), first_frame_rendered_ ? "Native TUI render failed" : "Native TUI startup failed"));
        const auto weak = weak_from_this();
        boost::asio::post(executor_, [weak] {
            if (const auto self = weak.lock()) self->request_exit();
        });
        return;
    }

    first_frame_rendered_ = true;
    ++render_count_;
    prompt_presentation_dirty_.store(false, std::memory_order_release);
    if (prepared_counted_frame_ && session_ui_ != nullptr) {
        session_ui_->clear_pending_counted_frame();
    }
    prepared_counted_frame_ = false;
}

void InteractiveEngine::request_exit() {
    if (!running_ || exit_requested_) return;
    exit_requested_ = true;
    if (session_ != nullptr) session_->close();
    if (!prompt_active_ && !user_bash_active_ && !compaction_active_) {
        signal_exit();
    }
}

void InteractiveEngine::signal_exit() {
    if (view_ != nullptr && session_ != nullptr && session_ui_ != nullptr) {
        (void)session_ui_->drain_composed_snapshot();
        if (session_ui_->pending_counted_frame() && session_ui_->reconcile_snapshot(session_ui_->composed_snapshot())) {
            prompt_presentation_dirty_.store(true, std::memory_order_release);
        }
    }
    if (prompt_presentation_dirty_.load(std::memory_order_acquire)) tui_.request_render(true);
    (void)exit_wait_.cancel();
}

} // namespace cch::coding_agent::tui

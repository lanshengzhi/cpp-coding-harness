// InteractiveEngine collaborator-wiring unit (#506): the factories that
// wire the ModalFlowControllers (Model/Auth/Session/Settings), the
// SessionUiBinding adapter, and the SuspendController to the engine's
// executor, lifetime, and gate seams, plus the detached-flow spawn
// machinery those hooks post through. See InteractiveEngine.hpp for the
// unit map.

#include "InteractiveEngine.hpp"

#include "support/AsyncResultBridge.hpp"
#include "coding_agent/tui/AuthFlowController.hpp"
#include "coding_agent/tui/InteractiveView.hpp"
#include "coding_agent/tui/ModelFlowController.hpp"
#include "coding_agent/tui/SessionFlowController.hpp"
#include "coding_agent/tui/SessionUiBinding.hpp"
#include "coding_agent/tui/SettingsFlowController.hpp"
#include "coding_agent/tui/SharedKeybindings.hpp"
#include "coding_agent/tui/SuspendController.hpp"
#include "coding_agent/tui/ThemeController.hpp"

#include <cch/tui/Terminal.hpp>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace cch::coding_agent::tui {

void InteractiveEngine::spawn_flow(
    std::move_only_function<boost::asio::awaitable<void>()> start,
    std::string failure_label) {
    ++in_flight_flows_;
    const auto weak = weak_from_this();
    // The coroutine lambda's frame may reference its closure (the
    // `start` move_only_function), so keep the closure alive until the
    // spawned coroutine reaches its terminal completion (the bridge holds
    // the factory for the coroutine's whole lifetime; ADR 0040
    // §Behavior mechanisms).
    auto start_owner =
        std::make_shared<std::move_only_function<boost::asio::awaitable<void>()>>(
            std::move(start));
    auto bridged = support::detail::make_async_result_on(
            executor_, [start_owner]() mutable -> boost::asio::awaitable<support::ExpectedVoid> {
                co_await (*start_owner)();
                co_return support::ExpectedVoid{};
            });
    std::move(bridged).start(
        [weak, failure_label = std::move(failure_label)](support::ExpectedVoid result) noexcept {
            if (const auto self = weak.lock()) {
                self->flow_finished();
                if (!result && self->running_ && self->view_ != nullptr) {
                    self->view_->append_diagnostic(std::move(failure_label));
                    self->tui_.invalidate();
                }
            }
        });
}

void InteractiveEngine::flow_finished() noexcept {
    if (in_flight_flows_ > 0) {
        --in_flight_flows_;
    }
    if (in_flight_flows_ == 0) {
        (void)flows_settled_.cancel();
    }
}

/// Wire the production host hooks for the model flows: gates read this
/// state's running/view facts, executor hops reuse post_from_view and
/// spawn_flow, and the current session resolves at execution time so
/// session replacement applies to flows in flight. All hooks capture the
/// state weakly; nothing here extends the state's lifetime.
std::shared_ptr<ModelFlowController> InteractiveEngine::make_model_flow_controller() {
    const auto weak = weak_from_this();
    ModelFlowHostHooks hooks;
    hooks.is_live = [weak] {
        const auto self = weak.lock();
        return self && self->running_ && self->view_ != nullptr;
    };
    hooks.post_on_executor = [weak](std::move_only_function<void()> action) mutable {
        if (const auto self = weak.lock()) {
            self->post_from_view(
                [action = std::move(action)](InteractiveEngine&) mutable { action(); });
        }
    };
    hooks.spawn_flow =
        [weak](std::move_only_function<boost::asio::awaitable<void>()> start,
               std::string failure_label) mutable {
            if (const auto self = weak.lock()) {
                self->spawn_flow(std::move(start), std::move(failure_label));
            }
        };
    hooks.current_session = [weak]() -> AgentSession* {
        const auto self = weak.lock();
        return self != nullptr ? self->session_ : nullptr;
    };
    // The controller is created after `theme_controller_` is emplaced and
    // the optional is never reset, so the pointer stays valid for the
    // state's lifetime (flows reach it only under the gates above).
    hooks.live_theme = [theme = &*theme_controller_]() -> const LiveTheme& {
        return theme->live_theme();
    };
    return std::make_shared<ModelFlowController>(
        executor_,
        *this,
        std::weak_ptr<void>{weak_from_this()},
        std::move(hooks),
        keybindings_,
        settings_manager_ ? &*settings_manager_ : nullptr);
}

std::shared_ptr<AuthFlowController> InteractiveEngine::make_auth_flow_controller() {
    const auto weak = weak_from_this();
    AuthFlowHostHooks hooks;
    hooks.post_on_executor = [weak](std::move_only_function<void()> action) mutable {
        if (const auto self = weak.lock()) {
            self->post_from_view(
                [action = std::move(action)](InteractiveEngine&) mutable { action(); });
        }
    };
    hooks.spawn_flow =
        [weak](std::move_only_function<boost::asio::awaitable<void>()> start,
               std::string failure_label) mutable {
            if (const auto self = weak.lock()) {
                self->spawn_flow(std::move(start), std::move(failure_label));
            }
        };
    hooks.current_session = [weak]() -> AgentSession* {
        const auto self = weak.lock();
        return self != nullptr ? self->session_ : nullptr;
    };
    hooks.live_theme = [theme = &*theme_controller_]() -> const LiveTheme& {
        return theme->live_theme();
    };
    hooks.action_generation = [weak] {
        const auto self = weak.lock();
        return self != nullptr ? self->action_generation_ : 0;
    };
    hooks.open_browser = [weak](std::size_t generation, std::string url) {
        if (const auto self = weak.lock()) {
            (void)self->deliver_action(
                generation,
                TuiActionVariant{OpenBrowserAction{std::move(url)}});
        }
    };
    return std::make_shared<AuthFlowController>(
        executor_,
        *this,
        std::weak_ptr<void>{weak_from_this()},
        std::move(hooks),
        keybindings_);
}

std::shared_ptr<SessionFlowController> InteractiveEngine::make_session_flow_controller() {
    const auto weak = weak_from_this();
    SessionFlowHostHooks hooks;
    hooks.is_live = [weak] {
        const auto self = weak.lock();
        return self && self->running_ && self->view_ != nullptr;
    };
    hooks.post_on_executor = [weak](std::move_only_function<void()> action) mutable {
        if (const auto self = weak.lock()) {
            self->post_from_view(
                [action = std::move(action)](InteractiveEngine&) mutable { action(); });
        }
    };
    hooks.spawn_flow =
        [weak](std::move_only_function<boost::asio::awaitable<void>()> start,
               std::string failure_label) mutable {
            if (const auto self = weak.lock()) {
                self->spawn_flow(std::move(start), std::move(failure_label));
            }
        };
    hooks.current_session = [weak]() -> AgentSession* {
        const auto self = weak.lock();
        return self != nullptr ? self->session_ : nullptr;
    };
    hooks.live_theme = [theme = &*theme_controller_]() -> const LiveTheme& {
        return theme->live_theme();
    };
    hooks.terminal_rows = [weak] {
        const auto self = weak.lock();
        return self != nullptr ? self->terminal_.dimensions().rows : 0;
    };
    hooks.action_generation = [weak] {
        const auto self = weak.lock();
        return self != nullptr ? self->action_generation_ : 0;
    };
    hooks.make_session_request = [weak](
        std::filesystem::path workspace,
        SessionTarget target) {
        if (const auto self = weak.lock()) {
            return self->make_session_request(
                std::move(workspace), std::move(target));
        }
        runtime::AgentSessionCreationRequest request;
        request.workspace = std::move(workspace);
        request.session_target = std::move(target);
        return request;
    };
    hooks.request_session_replacement = [weak](
        std::size_t generation,
        runtime::AgentSessionCreationRequest request)
        -> support::Expected<coding_agent::CreateAgentSessionResult> {
        if (const auto self = weak.lock()) {
            return self->request_session_replacement(
                generation, std::move(request));
        }
        return std::unexpected(support::make_error(
            support::ErrorCode::Cancelled,
            "Session flow host is no longer active"));
    };
    hooks.replace_session = [weak](std::unique_ptr<AgentSession> next)
        -> support::ExpectedVoid {
        if (const auto self = weak.lock()) {
            return self->replace_session(std::move(next));
        }
        return std::unexpected(support::make_error(
            support::ErrorCode::Cancelled,
            "Session flow host is no longer active"));
    };
    hooks.show_warning = [weak](std::string text) {
        if (const auto self = weak.lock(); self && self->view_ != nullptr) {
            self->view_->append_warning(std::move(text));
        }
    };
    hooks.show_frontend_message = [weak](std::string text) {
        if (const auto self = weak.lock(); self && self->view_ != nullptr) {
            self->view_->append_frontend_message(std::move(text));
        }
    };
    hooks.clear_status_indicator = [weak] {
        if (const auto self = weak.lock(); self && self->view_ != nullptr) {
            self->view_->clear_status_indicator();
        }
    };
    hooks.set_editor_text = [weak](std::string text) {
        if (const auto self = weak.lock(); self && self->view_ != nullptr) {
            self->view_->set_editor_text(std::move(text));
        }
    };
    hooks.editor_text = [weak] {
        if (const auto self = weak.lock(); self && self->view_ != nullptr) {
            return self->view_->editor_text();
        }
        return std::string{};
    };
    hooks.copy_to_clipboard = [weak](std::string text) {
        const auto self = weak.lock();
        return self != nullptr && self->write_clipboard_text_sink(text);
    };
    hooks.dequeue_pending_input = [weak] {
        if (const auto self = weak.lock()) self->dequeue_pending_input(false);
    };
    hooks.rebuild_chat = [weak] {
        if (const auto self = weak.lock()) self->rebuild_chat();
    };
    hooks.apply_reload_result = [weak](AgentSessionReloadResult result) -> support::ExpectedVoid {
        const auto self = weak.lock();
        if (!self) {
            return std::unexpected(support::make_error(
                support::ErrorCode::Cancelled,
                "Session flow host is no longer active"));
        }
        if (auto rebind = self->re_catalog_keybindings(); !rebind) {
            return std::unexpected(rebind.error());
        }
        if (self->theme_controller_) {
            auto discovery = coding_agent::tui::discover_themes(
                std::move(result.themes));
            self->loaded_theme_diagnostics_ = std::move(discovery.diagnostics);
            self->theme_controller_->set_registered_themes(
                std::move(discovery.themes));
            self->theme_controller_->apply_from_settings();
        }
        if (self->settings_manager_) {
            self->hide_thinking_block_ =
                self->settings_manager_->hide_thinking_block();
            self->output_pad_ = self->settings_manager_->output_pad();
        }
        if (self->view_ != nullptr) {
            self->view_->apply_render_settings(
                self->hide_thinking_block_, self->output_pad_);
        }
        self->rebuild_autocomplete_provider();
        self->refresh_loaded_resources();
        if (auto runtime = self->session_->model_runtime()) {
            if (auto error = runtime->get_error(); error && !error->empty()) {
                self->show_error("models.json error: " + *error);
            }
        }
        return {};
    };
    hooks.set_compaction_active = [weak](bool active) {
        if (const auto self = weak.lock()) self->compaction_active_ = active;
    };
    hooks.signal_exit = [weak] {
        if (const auto self = weak.lock(); self && self->exit_requested_ &&
            !self->prompt_active_ && !self->user_bash_active_ &&
            !self->compaction_active_) {
            self->signal_exit();
        }
    };
    hooks.request_exit = [weak] {
        if (const auto self = weak.lock()) self->post_exit();
    };
    hooks.report_boot_diagnostics = [weak](
        std::size_t generation,
        std::vector<SessionDiagnostic> diagnostics) {
        if (const auto self = weak.lock()) {
            (void)self->deliver_action(
                generation,
                TuiActionVariant{ReportBootDiagnosticsAction{
                    std::move(diagnostics)}});
        }
    };
    return std::make_shared<SessionFlowController>(
        executor_,
        *this,
        std::weak_ptr<void>{weak_from_this()},
        std::move(hooks),
        keybindings_,
        settings_manager_ ? &*settings_manager_ : nullptr);
}

std::shared_ptr<SessionUiBinding> InteractiveEngine::make_session_ui_binding() {
    const auto weak = weak_from_this();
    SessionUiBindingHooks hooks;
    hooks.is_live = [weak] {
        const auto self = weak.lock();
        return self && self->running_;
    };
    hooks.view = [weak]() -> InteractiveView* {
        const auto self = weak.lock();
        return self != nullptr ? self->view_ : nullptr;
    };
    hooks.prompt_active = [weak] {
        const auto self = weak.lock();
        return self && self->prompt_active_;
    };
    hooks.invalidate = [weak] {
        if (const auto self = weak.lock()) self->tui_.invalidate();
    };
    hooks.show_status = [weak](std::string text) {
        if (const auto self = weak.lock()) self->show_status(std::move(text));
    };
    hooks.show_error = [weak](std::string text) {
        if (const auto self = weak.lock()) self->show_error(std::move(text));
    };
    hooks.boot_workspace = [weak]() -> std::filesystem::path {
        const auto self = weak.lock();
        if (self != nullptr && self->boot_request_) {
            return self->boot_request_->workspace;
        }
        return {};
    };
    hooks.auto_compact_enabled = [weak]() -> std::optional<bool> {
        const auto self = weak.lock();
        if (!self || !self->settings_manager_) return std::nullopt;
        const auto compaction = self->settings_manager_->settings().compaction;
        return !compaction || compaction->enabled.value_or(true);
    };
    return std::make_shared<SessionUiBinding>(executor_, std::move(hooks));
}

std::shared_ptr<SettingsFlowController> InteractiveEngine::make_settings_flow_controller() {
    const auto weak = weak_from_this();
    SettingsFlowHostHooks hooks;
    hooks.is_live = [weak] {
        const auto self = weak.lock();
        return self && self->running_ && self->view_ != nullptr;
    };
    hooks.post_on_executor = [weak](std::move_only_function<void()> action) mutable {
        if (const auto self = weak.lock()) {
            self->post_from_view(
                [action = std::move(action)](InteractiveEngine&) mutable { action(); });
        }
    };
    hooks.current_session = [weak]() -> AgentSession* {
        const auto self = weak.lock();
        return self != nullptr ? self->session_ : nullptr;
    };
    hooks.overlay_active = [weak] {
        const auto self = weak.lock();
        return self && self->active_overlay_ != nullptr;
    };
    hooks.hide_thinking_block = [weak] {
        const auto self = weak.lock();
        return self && self->hide_thinking_block_;
    };
    hooks.output_pad = [weak] {
        const auto self = weak.lock();
        return self != nullptr ? self->output_pad_ : 0;
    };
    hooks.set_hide_thinking_block = [weak](bool hidden) {
        if (const auto self = weak.lock()) self->hide_thinking_block_ = hidden;
    };
    hooks.set_output_pad = [weak](std::size_t padding) {
        if (const auto self = weak.lock()) self->output_pad_ = padding;
    };
    hooks.rebuild_chat = [weak] {
        if (const auto self = weak.lock()) self->rebuild_chat();
    };
    hooks.rebuild_autocomplete_provider = [weak] {
        if (const auto self = weak.lock()) self->rebuild_autocomplete_provider();
    };
    return std::make_shared<SettingsFlowController>(
        executor_,
        *this,
        std::weak_ptr<void>{weak_from_this()},
        std::move(hooks),
        keybindings_,
        settings_manager_ ? &*settings_manager_ : nullptr,
        theme_controller_ ? theme_controller_.get() : nullptr);
}

std::shared_ptr<SuspendController> InteractiveEngine::make_suspend_controller() {
    const auto weak = weak_from_this();
    SuspendHooks hooks;
    hooks.stop_tui = [weak]() -> support::ExpectedVoid {
        if (const auto self = weak.lock()) return self->tui_.stop();
        return {};
    };
    hooks.start_tui = [weak]() -> support::ExpectedVoid {
        if (const auto self = weak.lock()) return self->tui_.start();
        return {};
    };
    hooks.render_tui = [weak]() -> support::ExpectedVoid {
        if (const auto self = weak.lock()) return self->tui_.render();
        return {};
    };
    hooks.report_failure = [weak](support::Error error) {
        if (const auto self = weak.lock()) {
            self->completion_result_ = std::unexpected(std::move(error));
            self->request_exit();
        }
    };
    hooks.suspend_process = [weak] {
        if (const auto self = weak.lock()) {
            (void)self->deliver_action(
                self->action_generation_,
                TuiActionVariant{SuspendProcessAction{}});
        }
    };
    hooks.is_running = [weak] {
        const auto self = weak.lock();
        return self && self->running_;
    };
    return std::make_shared<SuspendController>(executor_, std::move(hooks));
}

} // namespace cch::coding_agent::tui

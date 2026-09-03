// InteractiveEngine composition-host unit (#506): slash-command effects
// (the immediate-command handlers the router delegates to), in-session
// session replacement, and closed application-level action delivery to the
// composition host (ADR 0040). See InteractiveEngine.hpp for the unit map.

#include "InteractiveEngine.hpp"

#include "coding_agent/tui/AuthFlowController.hpp"
#include "coding_agent/tui/ClipboardWrite.hpp"
#include "coding_agent/tui/ErrorPresentation.hpp"
#include "coding_agent/tui/InteractiveView.hpp"
#include "coding_agent/tui/ModelFlowController.hpp"
#include "coding_agent/tui/OpenBrowser.hpp"
#include "coding_agent/tui/SessionFlowController.hpp"
#include "coding_agent/tui/SessionUiBinding.hpp"
#include "coding_agent/tui/SettingsFlowController.hpp"
#include "coding_agent/tui/SharedKeybindings.hpp"
#include "coding_agent/tui/SlashCommandEffects.hpp"
#include "support/AsyncResultBridge.hpp"

#include <csignal>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <unistd.h>

namespace cch::coding_agent::tui {
namespace {

/// `JSON.stringify`-shaped quoting for the `/name` normalization warning
/// (pi `JSON.stringify(name)`): a double-quoted literal with the JSON
/// escapes pi would emit.
[[nodiscard]] std::string json_quote_string(std::string_view value) {
    std::string result = "\"";
    for (const char character : value) {
        switch (character) {
        case '\\': result += "\\\\"; break;
        case '"': result += "\\\""; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default: result.push_back(character); break;
        }
    }
    result.push_back('"');
    return result;
}

} // namespace

using interactive_view_detail::queued_editor_texts;

void InteractiveEngine::show_help_command() {
    if (view_ == nullptr) return;
    view_->append_frontend_message(std::string{kHelpCommandText});
    tui_.invalidate();
}

void InteractiveEngine::open_hotkeys() {
    if (view_ == nullptr || !keybindings_) return;
    view_->append_frontend_message(format_hotkeys_text(*keybindings_->get()));
    tui_.invalidate();
}

void InteractiveEngine::handle_copy_last_message() {
    const auto text = session_->last_assistant_text();
    if (!text || text->empty()) {
        show_error("No agent messages to copy yet.");
        return;
    }
    if (!write_clipboard_text_sink(*text)) {
        show_error("Failed to copy to clipboard");
        return;
    }
    show_status("Copied last agent message to clipboard");
}

void InteractiveEngine::handle_name_command(std::string name) {
    if (name.empty()) {
        const auto current = session_->session_name();
        if (current && !current->empty()) {
            view_->append_frontend_message(
                std::format("Session name: {}", *current));
        } else {
            view_->append_warning("Usage: /name <name>");
        }
        tui_.invalidate();
        return;
    }
    auto stored = session_->set_session_name(name);
    if (!stored) {
        append_command_error(stored.error());
        return;
    }
    if (stored->has_value() && *stored != name) {
        // pi `showWarning("Session name was normalized from
        // ${JSON.stringify(name)} to ${JSON.stringify(sessionName)}")`.
        view_->append_warning(std::format(
            "Session name was normalized from {} to {}",
            json_quote_string(name),
            json_quote_string(**stored)));
    }
    view_->append_frontend_message(
        std::format("Session name set: {}", stored->value_or(name)));
    tui_.invalidate();
}

void InteractiveEngine::handle_session_command() {
    view_->append_frontend_message(format_session_info(*session_));
    tui_.invalidate();
}

bool InteractiveEngine::write_clipboard_text_sink(std::string text) {
    auto result = deliver_action(
        action_generation_,
        TuiActionVariant{WriteClipboardAction{std::move(text)}});
    if (!result) {
        return false;
    }
    const auto* wrote = std::get_if<bool>(&*result);
    return wrote != nullptr && *wrote;
}

runtime::AgentSessionCreationRequest InteractiveEngine::make_session_request(
    std::filesystem::path workspace,
    SessionTarget target) const {
    runtime::AgentSessionCreationRequest request;
    request.provide_user_shell = true;
    // pi `projectTrustByCwd`: the CLI override wins; otherwise the boot
    // decision applies to the boot workspace (a session-only trust
    // choice leaves no store entry and must survive in-session
    // replacement).
    request.project_trust_override =
        session_facts_.project_trust_override.has_value()
            ? session_facts_.project_trust_override
            : (resolved_boot_trust_ &&
                      resolved_boot_trust_->first == workspace
                  ? std::optional<bool>{resolved_boot_trust_->second}
                  : std::nullopt);
    request.session_facts = session_facts_;
    request.workspace = std::move(workspace);
    request.session_target = std::move(target);
    return request;
}

/// The error a host without the asynchronous replacement capability returns
/// to a Session replacement flow.
support::Error InteractiveEngine::session_replacement_unavailable_error() {
    return support::make_error(
        support::ErrorCode::Unknown,
        "Session switching is not available in this host");
}

support::Expected<TuiActionResultVariant> InteractiveEngine::deliver_action(
    std::size_t captured_generation,
    TuiActionVariant action) {
    if (captured_generation != action_generation_) {
        return std::unexpected(support::make_error(
            support::ErrorCode::Cancelled,
            "Native TUI action rejected",
            "retired session generation"));
    }
    if (action_sink_) {
        // The action is stamped with the generation that admitted it so
        // the host can reject a delivery from a retired generation.
        return action_sink_(action_generation_, std::move(action));
    }
    // Null host: TUI-local platform defaults for environment operations;
    // diagnostics and reporting are silent; replacement is unavailable.
    return std::visit(
        [](auto&& payload) -> support::Expected<TuiActionResultVariant> {
            using T = std::decay_t<decltype(payload)>;
            if constexpr (std::is_same_v<T, OpenBrowserAction>) {
                open_browser(std::move(payload.url));
                return TuiActionResultVariant{std::monostate{}};
            } else if constexpr (std::is_same_v<T, WriteClipboardAction>) {
                return TuiActionResultVariant{
                    write_clipboard_text(payload.text)};
            } else if constexpr (std::is_same_v<T, SuspendProcessAction>) {
                (void)::kill(0, SIGTSTP);
                return TuiActionResultVariant{std::monostate{}};
            } else {
                return TuiActionResultVariant{std::monostate{}};
            }
        },
        std::move(action));
}

boost::asio::awaitable<support::Expected<coding_agent::CreateAgentSessionResult>>
InteractiveEngine::request_session_replacement_async(
        std::size_t captured_generation, runtime::AgentSessionCreationRequest request, std::stop_token stop_token) {
    if (captured_generation != action_generation_) {
        co_return std::unexpected(support::make_error(
                support::ErrorCode::Cancelled, "Native TUI action rejected", "retired session generation"));
    }
    if (!async_session_replacement_sink_) {
        co_return std::unexpected(session_replacement_unavailable_error());
    }
    auto created = co_await support::detail::await_async_result(
            async_session_replacement_sink_(captured_generation, std::move(request), stop_token));
    if (captured_generation != action_generation_) {
        co_return std::unexpected(support::make_error(
                support::ErrorCode::Cancelled, "Native TUI action rejected", "retired session generation"));
    }
    co_return created;
}

support::ExpectedVoid InteractiveEngine::replace_session(
    std::unique_ptr<AgentSession> next) {
    retire_current_session();
    // The retired Session's in-flight prompt and User Bash can no longer
    // gate or render as the current Session: invalidate their interrupt
    // generations and clear the active-work facts, the working indicator,
    // and any pending-bash progress block (their stale completions are
    // dropped by the generation check).
    if (prompt_active_ || user_bash_active_ || compaction_active_) {
        note_prompt_finished();
        prompt_active_ = false;
        user_bash_active_ = false;
        // A retired Session's compaction keeps no current-Session event
        // subscription, so its end can never clear this fact here.
        compaction_active_ = false;
        if (view_ != nullptr) {
            view_->clear_status_indicator();
            view_->clear_user_bash_progress();
        }
        // Clearing the retired Session's active-work facts removes the
        // blockers a deferred exit was waiting on: a quit requested during
        // the transition must still release the exit wait once nothing is
        // active (the retired work's late completions early-return on the
        // generation check and never re-arm this).
        if (exit_requested_) {
            signal_exit();
        }
    }
    // Retain the replaced Session while its admitted work is still
    // settling; drop any whose Close already finalised (Closed with no
    // in-flight work), so retention is bounded by real quiescence rather
    // than growing with every replacement (ADR 0040).
    std::erase_if(retired_sessions_, [](const std::unique_ptr<AgentSession>& retired) {
        return !retired->is_open() && !retired->is_busy();
    });
    if (owned_session_) retired_sessions_.push_back(std::move(owned_session_));
    owned_session_ = std::move(next);
    session_ = owned_session_.get();
    model_flows_->update_model_completion();
    auto subscribed = session_ui_->bind(*session_);
    if (!subscribed) {
        return std::unexpected(subscribed.error());
    }
    // The new session's resources replace the loaded-resources block
    // (pi `showLoadedResources` after `renderCurrentSessionState`). The
    // theme re-registration gap (replacement drops the created session's
    // theme documents) is pre-existing and out of scope; the themes
    // section keeps the registered set.
    refresh_loaded_resources();
    rebuild_chat();
    return {};
}

support::ExpectedVoid InteractiveEngine::execute_immediate_slash_command(
    const SlashCommandInvocation& invocation) {
    switch (invocation.command) {
    case SlashCommandId::Clear:
        if (session_ == nullptr) {
            return std::unexpected(support::make_error(
                support::ErrorCode::Session,
                "No active session for /clear"));
        }
        session_flows_->open_new();
        return {};
    case SlashCommandId::Quit:
        if (view_ != nullptr) {
            tui_.invalidate();
            render();
        }
        request_exit();
        return {};
    case SlashCommandId::Copy:
        if (session_ == nullptr) {
            return std::unexpected(support::make_error(
                support::ErrorCode::Session,
                "No active session for /copy"));
        }
        handle_copy_last_message();
        return {};
    case SlashCommandId::Session:
        if (session_ == nullptr) {
            return std::unexpected(support::make_error(
                support::ErrorCode::Session,
                "No active session for /session"));
        }
        handle_session_command();
        return {};
    case SlashCommandId::Hotkeys:
        open_hotkeys();
        return {};
    case SlashCommandId::Settings:
        settings_flows_->show_settings_selector();
        return {};
    case SlashCommandId::Help:
        show_help_command();
        return {};
    case SlashCommandId::Name:
        if (session_ == nullptr) {
            return std::unexpected(support::make_error(
                support::ErrorCode::Session,
                "No active session for /name"));
        }
        handle_name_command(invocation.argument);
        return {};
    case SlashCommandId::Model:
    case SlashCommandId::Models:
    case SlashCommandId::Thinking:
    case SlashCommandId::Login:
    case SlashCommandId::Logout:
    case SlashCommandId::Resume:
    case SlashCommandId::Fork:
    case SlashCommandId::Tree:
    case SlashCommandId::Reload:
    case SlashCommandId::Compact:
    case SlashCommandId::Trust:
        return std::unexpected(support::make_error(
            support::ErrorCode::Validation,
            "Command is not an immediate slash command"));
    }
    return std::unexpected(support::make_error(
        support::ErrorCode::Validation,
        "Unknown immediate slash command"));
}

void InteractiveEngine::dispatch_modal_slash_command(SlashCommandInvocation invocation) {
    switch (invocation.command) {
    case SlashCommandId::Model:
        model_flows_->open_model_selector(std::move(invocation.argument));
        return;
    case SlashCommandId::Models:
        model_flows_->open_scoped_models_selector();
        return;
    case SlashCommandId::Thinking:
        if (!invocation.argument.empty()) {
            if (session_ == nullptr) {
                show_error("No active session for /thinking");
                return;
            }
            if (auto applied = session_->set_thinking_level(invocation.argument);
                !applied) {
                show_error(combined_error_text(applied.error()));
                return;
            }
        }
        settings_flows_->show_settings_selector();
        return;
    case SlashCommandId::Login:
        auth_flows_->open_login(std::move(invocation.argument));
        return;
    case SlashCommandId::Logout:
        auth_flows_->open_logout();
        return;
    case SlashCommandId::Resume:
        session_flows_->open_resume();
        return;
    case SlashCommandId::Fork:
        session_flows_->open_fork();
        return;
    case SlashCommandId::Tree:
        session_flows_->open_tree();
        return;
    case SlashCommandId::Reload:
        session_flows_->open_reload();
        return;
    case SlashCommandId::Compact:
        session_flows_->open_compact(std::move(invocation.argument));
        return;
    case SlashCommandId::Trust:
        session_flows_->open_trust();
        return;
    case SlashCommandId::Clear:
    case SlashCommandId::Quit:
    case SlashCommandId::Copy:
    case SlashCommandId::Session:
    case SlashCommandId::Hotkeys:
    case SlashCommandId::Settings:
    case SlashCommandId::Help:
    case SlashCommandId::Name:
        show_error(
            "Immediate slash command was routed as a modal command");
        return;
    }
}

void InteractiveEngine::dequeue_pending_input(bool announce) {
    if (!running_ || view_ == nullptr || !session_->is_open()) return;
    const auto snapshot = session_->snapshot();
    std::vector<std::string> restored;
    if (auto collected = queued_editor_texts(snapshot.agent_state.input_queues);
        !collected) {
        view_->append_diagnostic(collected.error().message);
        tui_.invalidate();
        return;
    } else {
        restored = std::move(*collected);
    }
    if (restored.empty()) {
        if (announce) view_->append_frontend_message("No queued messages to restore");
        session_ui_->sync_pending_input();
        tui_.invalidate();
        return;
    }
    if (auto cleared = session_->clear_input_queues(); !cleared) {
        view_->append_diagnostic(bounded_redacted_presentation(std::format(
            "Unable to restore queued input: {}",
            combined_error_text(cleared.error()))));
        session_ui_->sync_pending_input();
        tui_.invalidate();
        return;
    }
    view_->restore_queued_text(restored);
    if (announce) {
        view_->append_frontend_message(std::format(
            "Restored {} queued message{} to editor",
            restored.size(),
            restored.size() == 1 ? "" : "s"));
    }
    session_ui_->sync_pending_input();
    tui_.invalidate();
}

void InteractiveEngine::retire_current_session() noexcept {
    retire_action_generation();
    session_ui_->detach();
    if (session_ != nullptr) {
        session_->close();
    }
}

} // namespace cch::coding_agent::tui

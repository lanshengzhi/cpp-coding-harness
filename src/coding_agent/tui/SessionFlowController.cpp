#include "SessionFlowController.hpp"

#include "coding_agent/SessionCwd.hpp"
#include "coding_agent/SessionDiscovery.hpp"
#include "coding_agent/SessionPathPolicy.hpp"
#include "coding_agent/runtime/AgentSessionInteractiveAccess.hpp"
#include "coding_agent/tui/ErrorPresentation.hpp"
#include "coding_agent/tui/InteractiveView.hpp"
#include "coding_agent/tui/PromptSlot.hpp"
#include "coding_agent/tui/ReloadBox.hpp"
#include "coding_agent/tui/SessionSelector.hpp"
#include "coding_agent/tui/SharedKeybindings.hpp"
#include "coding_agent/tui/StringListSelector.hpp"
#include "coding_agent/tui/TreeSelector.hpp"
#include "coding_agent/tui/UserMessageSelector.hpp"
#include "support/AsyncResultBridge.hpp"

#include <cch/agent/harness/session/SessionStore.hpp>

#include <cch/coding_agent/AgentConfigDir.hpp>
#include <cch/coding_agent/Settings.hpp>
#include <cch/support/Error.hpp>

#include <boost/asio/redirect_error.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <algorithm>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace cch::coding_agent::tui {
namespace {

[[nodiscard]] support::Error prompt_cancelled_error() {
    return support::make_error(support::ErrorCode::Cancelled, "Login cancelled");
}

[[nodiscard]] support::Error session_replacement_unavailable_error() {
    return support::make_error(
        support::ErrorCode::Unknown,
        "Session switching is not available in this host");
}

} // namespace

SessionFlowController::SessionFlowController(
    boost::asio::any_io_executor executor,
    ModalPresenter& presenter,
    std::weak_ptr<void> host_lifetime,
    SessionFlowHostHooks hooks,
    std::shared_ptr<SharedKeybindings> keybindings,
    coding_agent::SettingsManager* settings_manager)
    : executor_(std::move(executor)),
      presenter_(&presenter),
      host_lifetime_(std::move(host_lifetime)),
      hooks_(std::move(hooks)),
      keybindings_(std::move(keybindings)),
      settings_manager_(settings_manager) {}

void SessionFlowController::open_resume() {
    auto self = shared_from_this();
    post([self] { self->show_session_selector(); });
}

void SessionFlowController::open_fork() {
    auto self = shared_from_this();
    post([self] { self->show_user_message_selector(); });
}

void SessionFlowController::open_new() {
    auto self = shared_from_this();
    post([self] {
        self->spawn(
            [self]() -> boost::asio::awaitable<void> {
                co_await self->handle_new_session();
            },
            "Native TUI new-session flow failed");
    });
}

void SessionFlowController::open_tree() {
    auto self = shared_from_this();
    post([self] { self->show_tree_selector(); });
}

void SessionFlowController::open_compact(std::string custom_instructions) {
    if (closed_ || hooks_.set_compaction_active == nullptr ||
        hooks_.post_on_executor == nullptr) {
        return;
    }
    auto self = shared_from_this();
    post([self, custom_instructions = std::move(custom_instructions)]() mutable {
        self->spawn(
            [self, custom_instructions = std::move(custom_instructions)]() mutable
                -> boost::asio::awaitable<void> {
                co_await self->handle_compact_command(std::move(custom_instructions));
            },
            "Native TUI compact flow failed");
    });
}

void SessionFlowController::open_reload() {
    auto self = shared_from_this();
    post([self] {
        self->spawn(
            [self]() -> boost::asio::awaitable<void> {
                co_await self->handle_reload();
            },
            "Native TUI reload flow failed");
    });
}

void SessionFlowController::open_trust() {
    auto self = shared_from_this();
    post([self] { self->show_trust_selector(); });
}

void SessionFlowController::close() {
    if (closed_.exchange(true)) return;
    stop_source_.request_stop();
    // Resolve every admitted prompt slot so each detached flow reaches a
    // terminal outcome and finish() can await quiescence (ADR 0040); a
    // second `/trust`/missing-cwd prompt may have admitted a concurrent
    // flow whose slot replaced the previous one.
    for (auto& slot : active_prompt_slots_) {
        slot->resolve(std::unexpected(
            support::make_error(support::ErrorCode::Cancelled, "Session flow cancelled")));
    }
    active_prompt_slots_.clear();
    presenter_->restore_prompt_slot();
}

void SessionFlowController::track_prompt_slot(std::shared_ptr<PromptSlot> slot) {
    active_prompt_slots_.push_back(std::move(slot));
}

void SessionFlowController::untrack_prompt_slot(const std::shared_ptr<PromptSlot>& slot) {
    std::erase(active_prompt_slots_, slot);
}

bool SessionFlowController::is_live() {
    return !closed_ && hooks_.is_live != nullptr && hooks_.is_live();
}

AgentSession* SessionFlowController::current_session() {
    return hooks_.current_session != nullptr ? hooks_.current_session() : nullptr;
}

std::size_t SessionFlowController::action_generation() {
    return hooks_.action_generation != nullptr ? hooks_.action_generation() : 0;
}

runtime::AgentSessionCreationRequest SessionFlowController::make_session_request(
    std::filesystem::path workspace,
    SessionTarget target) {
    if (hooks_.make_session_request != nullptr) {
        return hooks_.make_session_request(std::move(workspace), std::move(target));
    }
    runtime::AgentSessionCreationRequest request;
    request.workspace = std::move(workspace);
    request.session_target = std::move(target);
    return request;
}

void SessionFlowController::post(std::move_only_function<void()> action) {
    if (closed_) return;
    if (hooks_.post_on_executor != nullptr) {
        hooks_.post_on_executor(std::move(action));
    }
}

void SessionFlowController::spawn(
    std::move_only_function<boost::asio::awaitable<void>()> start,
    std::string failure_label) {
    if (closed_ || hooks_.spawn_flow == nullptr) return;
    auto self = shared_from_this();
    auto host_lifetime = host_lifetime_.lock();
    if (host_lifetime == nullptr) return;
    hooks_.spawn_flow(
        [self, host_lifetime = std::move(host_lifetime), start = std::move(start)]()
            mutable -> boost::asio::awaitable<void> {
            co_await start();
        },
        std::move(failure_label));
}

support::Expected<coding_agent::CreateAgentSessionResult>
SessionFlowController::request_session_replacement(
    runtime::AgentSessionCreationRequest request) {
    if (hooks_.request_session_replacement == nullptr) {
        return std::unexpected(session_replacement_unavailable_error());
    }
    return hooks_.request_session_replacement(action_generation(), std::move(request));
}

boost::asio::awaitable<support::Expected<coding_agent::CreateAgentSessionResult>>
SessionFlowController::request_session_replacement_async(runtime::AgentSessionCreationRequest request) {
    const auto captured_generation = action_generation();
    if (hooks_.request_session_replacement_async != nullptr) {
        auto created = co_await support::detail::await_async_result(hooks_.request_session_replacement_async(
                captured_generation, std::move(request), stop_source_.get_token()));
        if (closed_ || captured_generation != action_generation()) {
            co_return std::unexpected(
                    support::make_error(support::ErrorCode::Cancelled, "Session replacement was superseded"));
        }
        co_return created;
    }
    if (hooks_.request_session_replacement != nullptr) {
        if (closed_ || stop_source_.stop_requested() || captured_generation != action_generation()) {
            co_return std::unexpected(
                    support::make_error(support::ErrorCode::Cancelled, "Session replacement was superseded"));
        }
        auto created = hooks_.request_session_replacement(captured_generation, std::move(request));
        if (closed_ || stop_source_.stop_requested() || captured_generation != action_generation()) {
            co_return std::unexpected(
                    support::make_error(support::ErrorCode::Cancelled, "Session replacement was superseded"));
        }
        co_return created;
    }
    co_return std::unexpected(session_replacement_unavailable_error());
}

support::ExpectedVoid SessionFlowController::replace_session(
    std::unique_ptr<AgentSession> session) {
    if (hooks_.replace_session == nullptr) {
        return std::unexpected(session_replacement_unavailable_error());
    }
    return hooks_.replace_session(std::move(session));
}

boost::asio::awaitable<void> SessionFlowController::handle_resume_session(
    std::string session_path) {
    auto* session = current_session(); // borrowed; host retains retired sessions.
    if (session == nullptr) co_return;
    const auto captured_generation = action_generation();
    const auto fallback_cwd = session->workspace();
    if (hooks_.request_session_replacement == nullptr && hooks_.request_session_replacement_async == nullptr) {
        presenter_->show_error("Session switching is not available in this host");
        co_return;
    }
    // pi `SessionManager.open` reads the header cwd first; a stored cwd that
    // no longer exists prompts (`assertSessionCwdExists`).
    const auto target = std::filesystem::path{session_path};
    std::optional<std::filesystem::path> header_cwd;
    if (auto info = session_discovery::build_session_info(target);
        info && !info->cwd.empty()) {
        header_cwd = std::filesystem::path{info->cwd};
    }
    std::optional<std::filesystem::path> cwd_override;
    if (header_cwd) {
        std::error_code exists_ec;
        if (!std::filesystem::exists(*header_cwd, exists_ec)) {
            auto chosen = co_await prompt_for_missing_session_cwd(
                *header_cwd, fallback_cwd);
            if (closed_ || captured_generation != action_generation()) co_return;
            if (!chosen) {
                presenter_->show_status("Resume cancelled");
                co_return;
            }
            cwd_override = chosen;
        }
    }

    if (closed_ || captured_generation != action_generation()) co_return;
    session = current_session();
    if (session == nullptr) co_return;
    auto request = make_session_request(
        cwd_override ? *cwd_override : session->workspace(),
        ExplicitResumeSessionTarget{target});
    request.resume_cwd_override = cwd_override;
    auto created = co_await request_session_replacement_async(std::move(request));
    if (!created) {
        presenter_->show_error(combined_error_text(created.error()));
        co_return;
    }
    if (auto replaced = replace_session(std::move(created->session)); !replaced) {
        presenter_->show_error(combined_error_text(replaced.error()));
        co_return;
    }
    if (closed_) co_return;
    presenter_->show_status(
        cwd_override ? "Resumed session in current cwd" : "Resumed session");
}

boost::asio::awaitable<void> SessionFlowController::handle_new_session() {
    auto* session = current_session();
    if (session == nullptr) co_return;
    if (hooks_.request_session_replacement == nullptr && hooks_.request_session_replacement_async == nullptr) {
        presenter_->show_error("Session switching is not available in this host");
        co_return;
    }
    auto request = make_session_request(
        session->workspace(),
        session->session_path()
            ? SessionTarget{DefaultPersistedSessionTarget{}}
            : SessionTarget{InMemorySessionTarget{}});
    if (session->session_path()) {
        request.session_dir = session->session_path()->parent_path().string();
    }
    auto created = co_await request_session_replacement_async(std::move(request));
    if (!created) {
        presenter_->show_error(combined_error_text(created.error()));
        co_return;
    }
    if (auto replaced = replace_session(std::move(created->session)); !replaced) {
        presenter_->show_error(combined_error_text(replaced.error()));
        co_return;
    }
    if (hooks_.show_frontend_message != nullptr) {
        hooks_.show_frontend_message("✓ New session started");
    }
    presenter_->invalidate();
}

void SessionFlowController::show_user_message_selector() {
    if (!is_live()) return;
    auto* session = current_session();
    if (session == nullptr || !session->is_open() || hooks_.live_theme == nullptr ||
        keybindings_ == nullptr) {
        return;
    }
    const auto messages = session->get_user_messages_for_forking();
    if (messages.empty()) {
        presenter_->show_status("No messages to fork from");
        return;
    }
    const auto initial_selected_id = messages.back().entry_id;
    std::vector<UserForkItem> items;
    items.reserve(messages.size());
    for (const auto& message : messages) {
        items.push_back(UserForkItem{
            .entry_id = message.entry_id,
            .text = message.text,
        });
    }

    const auto weak = weak_from_this();
    auto selector = std::make_shared<UserMessageSelectorComponent>(
        hooks_.live_theme(),
        keybindings_->get(),
        std::move(items),
        initial_selected_id,
        [weak](std::string entry_id) {
            if (const auto self = weak.lock()) {
                self->post(
                    [self, entry_id = std::move(entry_id)]() mutable {
                        self->presenter_->restore_prompt_slot();
                        self->spawn(
                            [self, entry_id = std::move(entry_id)]() mutable
                                -> boost::asio::awaitable<void> {
                                co_await self->handle_fork_session(std::move(entry_id));
                            },
                            "Native TUI session fork flow failed");
                    });
            }
        },
        [weak] {
            if (const auto self = weak.lock()) {
                self->post([self] { self->presenter_->restore_prompt_slot(); });
            }
        },
        [weak] {
            if (const auto self = weak.lock()) self->presenter_->request_render();
        });
    presenter_->replace_prompt_slot(std::move(selector));
}

boost::asio::awaitable<void> SessionFlowController::handle_fork_session(
    std::string entry_id) {
    auto* session = current_session();
    if (session == nullptr) co_return;
    if (hooks_.request_session_replacement == nullptr && hooks_.request_session_replacement_async == nullptr) {
        presenter_->show_error("Session switching is not available in this host");
        co_return;
    }
    auto prepared = session->prepare_fork(entry_id, runtime::ForkPosition::Before);
    if (!prepared) {
        presenter_->show_error(combined_error_text(prepared.error()));
        co_return;
    }

    std::optional<std::string> selected_text = std::move(prepared->selected_text);
    auto request = make_session_request(
        session->workspace(),
        prepared->branched_path
            ? SessionTarget{ExplicitOpenOrCreateSessionTarget{
                  *prepared->branched_path}}
            : SessionTarget{InMemorySessionTarget{}});
    if (prepared->in_memory_seed) {
        request.in_memory_branch_seed = std::move(prepared->in_memory_seed);
    }
    auto created = co_await request_session_replacement_async(std::move(request));
    if (!created) {
        presenter_->show_error(combined_error_text(created.error()));
        co_return;
    }
    if (auto replaced = replace_session(std::move(created->session)); !replaced) {
        presenter_->show_error(combined_error_text(replaced.error()));
        co_return;
    }
    if (hooks_.set_editor_text != nullptr && selected_text && !selected_text->empty()) {
        hooks_.set_editor_text(std::move(*selected_text));
    }
    presenter_->show_status("Forked to new session");
}

boost::asio::awaitable<std::optional<std::filesystem::path>>
SessionFlowController::prompt_for_missing_session_cwd(
    std::filesystem::path session_cwd,
    std::filesystem::path fallback_cwd) {
    if (!is_live() || hooks_.live_theme == nullptr || keybindings_ == nullptr) {
        co_return std::nullopt;
    }
    const auto executor = co_await boost::asio::this_coro::executor;
    auto slot = std::make_shared<PromptSlot>(executor);
    track_prompt_slot(slot);
    const auto prompt_text = format_missing_session_cwd_prompt(
        MissingSessionCwdIssue{
            .session_file = {},
            .session_cwd = session_cwd,
            .fallback_cwd = fallback_cwd,
        });
    const auto title = "Session cwd not found\n" + prompt_text;
    auto selector = std::make_shared<StringListSelector>(
        hooks_.live_theme(),
        keybindings_->get(),
        title,
        std::vector<std::string>{"Yes", "No"},
        [slot, fallback_cwd](std::string selected) {
            slot->resolve(selected == "Yes"
                ? support::Expected<std::string>{fallback_cwd.string()}
                : std::unexpected(prompt_cancelled_error()));
        },
        [slot] { slot->resolve(std::unexpected(prompt_cancelled_error())); });
    presenter_->replace_prompt_slot(std::move(selector));

    boost::system::error_code error;
    auto result = co_await slot->channel.async_receive(
        boost::asio::redirect_error(boost::asio::use_awaitable, error));
    untrack_prompt_slot(slot);
    if (closed_) co_return std::nullopt;
    if (error) {
        presenter_->restore_prompt_slot();
        co_return std::nullopt;
    }
    presenter_->restore_prompt_slot();
    if (!result) co_return std::nullopt;
    co_return std::filesystem::path{*result};
}

void SessionFlowController::show_session_selector() {
    if (!is_live()) return;
    auto* session = current_session();
    if (session == nullptr || !session->is_open() || hooks_.live_theme == nullptr ||
        keybindings_ == nullptr) {
        return;
    }
    const auto workspace = session->workspace();
    std::optional<std::filesystem::path> session_dir;
    if (auto path = session->session_path()) session_dir = path->parent_path();
    const auto sessions_root = coding_agent::sessions_root_path();
    const auto default_dir =
        sessions_root / session_paths::encode_workspace_key(workspace);
    const bool uses_default = session_dir && *session_dir == default_dir;
    const auto cwd_filter = [&]() -> std::optional<std::filesystem::path> {
        if (!session_dir) return std::nullopt;
        return uses_default ? std::nullopt
                            : std::optional<std::filesystem::path>{workspace};
    }();
    const auto loader_dir = session_dir.value_or(std::filesystem::path{});
    const auto current_loader =
        [loader_dir, cwd_filter]() -> std::vector<session_discovery::SessionInfo> {
        if (loader_dir.empty()) return {};
        return session_discovery::list_sessions_info(loader_dir, cwd_filter);
    };
    const auto all_loader =
        [sessions_root, session_dir, uses_default]()
        -> std::vector<session_discovery::SessionInfo> {
        if (!session_dir) return {};
        return session_discovery::list_all_sessions_info(
            sessions_root,
            uses_default ? std::nullopt : session_dir);
    };

    const auto weak = weak_from_this();
    auto selector = std::make_shared<SessionSelectorComponent>(
        hooks_.live_theme(),
        keybindings_->get(),
        current_loader,
        all_loader,
        session->session_path(),
        [weak](std::string session_path) {
            if (const auto self = weak.lock()) {
                self->post(
                    [self, session_path = std::move(session_path)]() mutable {
                        self->presenter_->restore_prompt_slot();
                        self->spawn(
                            [self, session_path = std::move(session_path)]() mutable
                                -> boost::asio::awaitable<void> {
                                co_await self->handle_resume_session(
                                    std::move(session_path));
                            },
                            "Native TUI session resume flow failed");
                    });
            }
        },
        [weak] {
            if (const auto self = weak.lock()) {
                self->post([self] { self->presenter_->restore_prompt_slot(); });
            }
        },
        [weak] {
            if (const auto self = weak.lock();
                self && self->hooks_.request_exit != nullptr) {
                self->hooks_.request_exit();
            }
        },
        [weak](std::string session_path, std::string name) -> support::ExpectedVoid {
            if (weak.expired()) {
                return std::unexpected(support::make_error(
                    support::ErrorCode::Cancelled,
                    "Session selector is no longer active"));
            }
            auto opened = harness::session::SessionStore::open_existing(
                std::filesystem::path{session_path});
            if (!opened) return std::unexpected(opened.error());
            return opened->append_session_info(std::nullopt, name);
        },
        [weak] {
            if (const auto self = weak.lock()) self->presenter_->request_render();
        });
    presenter_->replace_prompt_slot(std::move(selector));
}

void SessionFlowController::show_tree_selector() {
    if (!is_live()) return;
    auto* session = current_session();
    if (session == nullptr || !session->is_open() || hooks_.live_theme == nullptr ||
        keybindings_ == nullptr) {
        return;
    }
    auto topology = session->session_tree();
    if (!topology) {
        presenter_->show_error(combined_error_text(topology.error()));
        return;
    }
    if (topology->roots.empty()) {
        presenter_->show_status("No entries in session");
        return;
    }
    const auto leaf_id = topology->leaf_id;
    const auto weak = weak_from_this();
    auto selector = std::make_shared<TreeSelectorComponent>(
        hooks_.live_theme(),
        keybindings_->get(),
        std::move(topology->roots),
        topology->leaf_id,
        hooks_.terminal_rows != nullptr ? hooks_.terminal_rows() : 0,
        [weak, leaf_id](std::string entry_id) {
            if (const auto self = weak.lock()) {
                self->post(
                    [self, entry_id = std::move(entry_id), leaf_id]() mutable {
                        self->presenter_->restore_prompt_slot();
                        if (entry_id == leaf_id) {
                            self->presenter_->show_status("Already at this point");
                            return;
                        }
                        self->spawn(
                            [self, entry_id = std::move(entry_id)]() mutable
                                -> boost::asio::awaitable<void> {
                                co_await self->handle_tree_navigation(
                                    std::move(entry_id));
                            },
                            "Native TUI tree navigation flow failed");
                    });
            }
        },
        [weak] {
            if (const auto self = weak.lock()) {
                self->post([self] { self->presenter_->restore_prompt_slot(); });
            }
        },
        [weak](std::string entry_id, std::optional<std::string> label) {
            if (const auto self = weak.lock()) {
                self->post(
                    [self, entry_id = std::move(entry_id), label = std::move(label)]() mutable {
                        auto* session = self->current_session();
                        if (session == nullptr) return;
                        if (auto applied = session->set_entry_label(
                                entry_id, std::move(label));
                            !applied) {
                            self->presenter_->show_error(combined_error_text(applied.error()));
                        }
                    });
            }
        },
        [weak](std::optional<std::string> text) {
            if (const auto self = weak.lock()) {
                self->post(
                    [self, text = std::move(text)]() mutable {
                        self->handle_tree_copy(std::move(text));
                    });
            }
        },
        [weak] {
            if (const auto self = weak.lock()) self->presenter_->request_render();
        });
    presenter_->replace_prompt_slot(std::move(selector));
}

void SessionFlowController::handle_tree_copy(std::optional<std::string> text) {
    if (!text || text->empty()) {
        presenter_->show_error("Selected entry has no text to copy");
        return;
    }
    if (hooks_.copy_to_clipboard == nullptr || !hooks_.copy_to_clipboard(std::move(*text))) {
        presenter_->show_error("Failed to copy to clipboard");
        return;
    }
    presenter_->show_status("Copied selected message to clipboard");
}

boost::asio::awaitable<void> SessionFlowController::handle_tree_navigation(
    std::string entry_id) {
    auto* session = current_session(); // borrowed; retained through wait.
    if (session == nullptr) co_return;
    const auto captured_generation = action_generation();
    if (session->is_busy()) {
        // pi stops the active response first (restore queued input, abort,
        // wait for settle) before navigating.
        if (hooks_.dequeue_pending_input != nullptr) {
            hooks_.dequeue_pending_input();
        }
        session->abort();
        (void)co_await session->wait_for_idle();
        if (closed_ || captured_generation != action_generation()) co_return;
        session = current_session();
        if (session == nullptr) co_return;
    }
    if (closed_ || captured_generation != action_generation()) co_return;
    auto result = session->navigate_tree(entry_id);
    if (!result) {
        presenter_->show_error(combined_error_text(result.error()));
        co_return;
    }
    if (hooks_.rebuild_chat != nullptr) hooks_.rebuild_chat();
    if (result->editor_text && !result->editor_text->empty() &&
        hooks_.editor_text != nullptr && hooks_.set_editor_text != nullptr) {
        auto current = hooks_.editor_text();
        const auto first = current.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) {
            hooks_.set_editor_text(std::move(*result->editor_text));
        }
    }
    presenter_->show_status("Navigated to selected point");
}

boost::asio::awaitable<void> SessionFlowController::handle_compact_command(
    std::string custom_instructions) {
    auto* session = current_session(); // borrowed; retained through this flow.
    if (session == nullptr) co_return;
    // Admit the manual compaction only once the flow is actually running (a
    // dropped post/spawn or null session above must not strand host exit on
    // the active flag; ADR 0040 admission).
    if (hooks_.set_compaction_active != nullptr) {
        hooks_.set_compaction_active(true);
    }
    const auto captured_generation = action_generation();
    if (hooks_.clear_status_indicator != nullptr) {
        hooks_.clear_status_indicator();
        presenter_->invalidate();
    }
    // pi: failures are ignored — they surface through compaction session
    // events (`compaction_end`), so no error is reported here.
    static_cast<void>(co_await session->compact(std::move(custom_instructions)));
    if (hooks_.set_compaction_active != nullptr) {
        hooks_.set_compaction_active(false);
    }
    if (closed_ || captured_generation != action_generation()) co_return;
    if (hooks_.signal_exit != nullptr) hooks_.signal_exit();
}

boost::asio::awaitable<void> SessionFlowController::handle_reload() {
    auto* session = current_session(); // borrowed; retained through reload.
    if (!is_live() || session == nullptr) co_return;
    const auto captured_generation = action_generation();
    if (session->is_streaming()) {
        if (hooks_.show_warning != nullptr) {
            hooks_.show_warning(
                "Wait for the current response to finish before reloading.");
        }
        presenter_->invalidate();
        co_return;
    }
    if (session->is_compacting()) {
        if (hooks_.show_warning != nullptr) {
            hooks_.show_warning(
                "Wait for compaction to finish before reloading.");
        }
        presenter_->invalidate();
        co_return;
    }

    presenter_->replace_prompt_slot(make_reload_box(hooks_.live_theme()));
    auto result = co_await session->reload();
    if (closed_ || captured_generation != action_generation()) co_return;
    if (!result) {
        presenter_->restore_prompt_slot();
        presenter_->show_error("Reload failed: " + result.error().message);
        co_return;
    }
    if (hooks_.apply_reload_result == nullptr) {
        presenter_->restore_prompt_slot();
        presenter_->show_error("Reload failed: reload host is unavailable");
        co_return;
    }
    if (auto applied = hooks_.apply_reload_result(std::move(*result)); !applied) {
        presenter_->restore_prompt_slot();
        presenter_->show_error("Reload failed: " + applied.error().message);
        co_return;
    }

    auto saved_implicit_trust = co_await maybe_save_implicit_project_trust_after_reload();
    if (!saved_implicit_trust && saved_implicit_trust.error().code != support::ErrorCode::Cancelled && !closed_) {
        if (hooks_.show_warning != nullptr) {
            hooks_.show_warning("Could not determine project trust after reload: " +
                                combined_error_text(saved_implicit_trust.error()));
        }
        presenter_->invalidate();
    }
    presenter_->show_status(
            saved_implicit_trust && *saved_implicit_trust
                    ? "Reloaded keybindings, skills, prompts, themes, and context files; saved project trust"
                    : "Reloaded keybindings, skills, prompts, themes, and context files");
    presenter_->restore_prompt_slot();
}

} // namespace cch::coding_agent::tui

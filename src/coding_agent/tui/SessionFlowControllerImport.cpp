// pi v1.3.0 `/import` (spec #1044): the import flow of the Native TUI.
//
// pi's `handleImportCommand` (interactive-mode.ts:6585-6625) asks for a
// confirmation, then hands the path to `AgentSessionRuntime.importFromJsonl`
// (agent-session-runtime.ts:359-406), which copies the JSONL file into the
// session directory, opens it, and switches the runtime to it.
//
// The C++ flow reuses the ordinary session replacement (`/resume`'s
// machinery): the imported file is opened as an explicit resume target and
// installed through the same asynchronous Session Assembly door, so the
// imported session is genuinely the current session afterwards.

#include "SessionFlowController.hpp"

#include "coding_agent/SessionCwd.hpp"
#include "coding_agent/SessionDiscovery.hpp"
#include "coding_agent/SessionPathPolicy.hpp"
#include "coding_agent/tui/ErrorPresentation.hpp"
#include "coding_agent/tui/InteractiveView.hpp"
#include "coding_agent/tui/KeybindingHints.hpp"
#include "coding_agent/tui/PromptSlot.hpp"
#include "coding_agent/tui/SelectListPrompt.hpp"
#include "coding_agent/tui/SessionImport.hpp"
#include "coding_agent/tui/SharedKeybindings.hpp"
#include "coding_agent/tui/Theme.hpp"

#include <cch/coding_agent/AgentConfigDir.hpp>
#include <cch/support/Error.hpp>
#include <cch/tui/SelectList.hpp>

#include <boost/asio/redirect_error.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

namespace cch::coding_agent::tui {
namespace {

[[nodiscard]] support::Error prompt_cancelled_error() {
    return support::make_error(support::ErrorCode::Cancelled, "Login cancelled");
}

/// pi `resolvePath`: a leading `~` expands against the home directory, a
/// relative path resolves against the working directory, and the result is
/// absolute.
[[nodiscard]] std::filesystem::path resolve_import_path(
        const std::string& value, const std::filesystem::path& workspace) {
    return session_paths::resolve_session_path_value(value, workspace, coding_agent::home_directory());
}

/// pi `sessionManager.getSessionDir()`: the directory the current session
/// lives in. A persisted session's own directory is its file's parent; an
/// in-memory session has no directory, so the workspace-keyed default under
/// the Agent Config Directory sessions root applies.
[[nodiscard]] std::filesystem::path import_session_directory(const AgentSession& session) {
    if (const auto path = session.session_path()) {
        return path->parent_path();
    }
    return coding_agent::sessions_root_path() / coding_agent::session_paths::encode_workspace_key(session.workspace());
}

} // namespace

void SessionFlowController::open_import(std::string path) {
    if (closed_) return;
    auto self = shared_from_this();
    post([self, path = std::move(path)]() mutable {
        self->spawn(
                [self, path = std::move(path)]() mutable -> boost::asio::awaitable<void> {
                    co_await self->handle_import_session(std::move(path));
                },
                "Native TUI session import flow failed");
    });
}

boost::asio::awaitable<bool> SessionFlowController::confirm_import_session(std::string import_path) {
    if (!is_live() || hooks_.live_theme == nullptr || keybindings_ == nullptr) {
        co_return false;
    }
    const auto executor = co_await boost::asio::this_coro::executor;
    auto slot = std::make_shared<PromptSlot>(executor);
    track_prompt_slot(slot);
    auto list = std::make_unique<cch::tui::SelectList>(
            std::vector<cch::tui::SelectItem>{
                    {.value = "Yes", .label = "Yes"},
                    {.value = "No", .label = "No"},
            },
            cch::tui::SelectListOptions{
                    .theme = hooks_.live_theme().select_list_theme(),
                    .on_select = [slot](const cch::tui::SelectItem& item) -> support::ExpectedVoid {
                        slot->resolve(item.value == "Yes" ? support::Expected<std::string>{"Yes"}
                                                          : std::unexpected(prompt_cancelled_error()));
                        return {};
                    },
                    .on_cancel = [slot]() -> support::ExpectedVoid {
                        slot->resolve(std::unexpected(prompt_cancelled_error()));
                        return {};
                    },
                    .keybindings = keybindings_->get(),
            });
    // pi `showExtensionConfirm("Import session", "Replace current session
    // with ${inputPath}?")`: the title and the message share one selector
    // body.
    const auto title = std::format("Import session\nReplace current session with {}?", import_path);
    auto selector = std::make_shared<SelectListPrompt>(title,
            generic_select_list_hint(*keybindings_->get()),
            hooks_.live_theme().foreground_hook(ThemeToken::Border),
            std::move(list));
    presenter_->replace_prompt_slot(std::move(selector));

    boost::system::error_code error;
    auto result = co_await slot->channel.async_receive(boost::asio::redirect_error(boost::asio::use_awaitable, error));
    untrack_prompt_slot(slot);
    if (closed_) co_return false;
    presenter_->restore_prompt_slot();
    if (error || !result) co_return false;
    co_return *result == "Yes";
}

boost::asio::awaitable<void> SessionFlowController::handle_import_session(std::string import_path) {
    auto* session = current_session(); // borrowed; host retains retired sessions.
    if (session == nullptr) co_return;
    const auto captured_generation = action_generation();
    const auto fallback_cwd = session->workspace();
    if (hooks_.request_session_replacement_async == nullptr) {
        presenter_->show_error("Session switching is not available in this host");
        co_return;
    }

    // pi `handleImportCommand`: the confirmation precedes every file
    // operation, so a declined import changes nothing at all.
    const auto confirmed = co_await confirm_import_session(import_path);
    if (closed_ || captured_generation != action_generation()) co_return;
    if (!confirmed) {
        presenter_->show_status("Import cancelled");
        co_return;
    }

    const auto resolved = resolve_import_path(import_path, fallback_cwd);
    std::error_code exists_ec;
    if (!std::filesystem::exists(resolved, exists_ec)) {
        // pi `SessionImportFileNotFoundError`: a non-fatal chat error.
        presenter_->show_error(
                std::format("Failed to import session: {}", session_import_file_not_found_error(resolved).message));
        co_return;
    }

    // pi `importFromJsonl`: the file is copied into the session directory
    // (with pi's `-1`, `-2`, ... collision suffix) and opened from there.
    const auto session_dir = import_session_directory(*session);
    auto copied = copy_imported_session(session_dir, resolved);
    if (!copied) {
        presenter_->show_error(std::format("Failed to import session: {}", combined_error_text(copied.error())));
        co_return;
    }
    const auto destination = *copied;

    // pi `assertSessionCwdExists`: an imported session whose recorded cwd no
    // longer exists prompts for the current cwd before the retry.
    std::optional<std::filesystem::path> cwd_override;
    std::optional<std::filesystem::path> header_cwd;
    if (auto info = session_discovery::build_session_info(destination); info && !info->cwd.empty()) {
        header_cwd = std::filesystem::path{info->cwd};
    }
    if (header_cwd) {
        std::error_code cwd_ec;
        if (!std::filesystem::exists(*header_cwd, cwd_ec)) {
            auto chosen = co_await prompt_for_missing_session_cwd(*header_cwd, fallback_cwd);
            if (closed_ || captured_generation != action_generation()) co_return;
            if (!chosen) {
                presenter_->show_status("Import cancelled");
                co_return;
            }
            cwd_override = chosen;
        }
    }

    if (closed_ || captured_generation != action_generation()) co_return;
    session = current_session();
    if (session == nullptr) co_return;
    auto request = make_session_request(cwd_override ? *cwd_override : session->workspace(),
            SessionTarget{ExplicitResumeSessionTarget{destination}});
    request.resume_cwd_override = cwd_override;
    auto created = co_await request_session_replacement_async(std::move(request), captured_generation);
    if (!created) {
        release_session_replacement();
        presenter_->show_error(std::format("Failed to import session: {}", combined_error_text(created.error())));
        co_return;
    }
    auto replaced = replace_session(std::move(created->session));
    release_session_replacement();
    if (!replaced) {
        presenter_->show_error(std::format("Failed to import session: {}", combined_error_text(replaced.error())));
        co_return;
    }
    presenter_->show_status(std::format("Session imported from: {}", import_path));
    presenter_->invalidate();
}

} // namespace cch::coding_agent::tui

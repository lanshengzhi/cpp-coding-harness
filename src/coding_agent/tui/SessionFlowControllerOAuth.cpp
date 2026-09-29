// The Native TUI's browser authorization for one Upstream MCP Server
// (`/mcp auth <server>`, issue #849; spec #833 stories 34 and 35). Split out
// of SessionFlowController.cpp beside the trust flow it mirrors: the flow
// itself is the MCP Host's, and this is the frontend's half — showing the
// authorization URL, waiting for the user, and reporting how it ended.

#include "SessionFlowController.hpp"

#include "coding_agent/tui/ErrorPresentation.hpp"
#include "coding_agent/tui/KeybindingHints.hpp"
#include "coding_agent/tui/ModalPresenter.hpp"
#include "coding_agent/tui/PromptSlot.hpp"
#include "coding_agent/tui/SharedKeybindings.hpp"
#include "coding_agent/tui/Theme.hpp"
#include "support/AsyncResultBridge.hpp"

#include <boost/asio/redirect_error.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <system_error>
#include <utility>

namespace cch::coding_agent::tui {
namespace {

[[nodiscard]] support::Error oauth_dismissed_error() {
    return support::make_error(support::ErrorCode::Cancelled, "The Upstream MCP Server authorization was dismissed");
}

[[nodiscard]] support::Error oauth_unavailable_error() {
    return support::make_error(support::ErrorCode::Cancelled,
            "The Upstream MCP Server authorization prompt is not available",
            "the Native TUI is not live");
}

/// The one line a user sees for each terminal outcome. The text is a constant
/// per case plus the Server Id: no Upstream-supplied text and no token
/// reaches the chat scrollback, a diagnostic, or a session record.
[[nodiscard]] std::string oauth_status_message(const McpOAuthOutcome& outcome) {
    switch (outcome.status) {
    case McpOAuthStatus::Authorized:
        return "Authorized Upstream MCP Server " + outcome.server_id;
    case McpOAuthStatus::Cancelled:
        return "Authorization dismissed for Upstream MCP Server " + outcome.server_id;
    case McpOAuthStatus::Failed:
        break;
    }
    return "Could not authorize Upstream MCP Server " + outcome.server_id;
}

} // namespace

support::AsyncResult<void> SessionFlowController::ask_mcp_oauth_prompt(
        McpOAuthRequest request, std::stop_token stop_token) {
    auto self = shared_from_this();
    // Presented on this controller's own executor — the presenter and its
    // prompt slot are executor-confined — and the terminal value travels back
    // to the MCP Host's domain through the operation's own completion.
    return support::detail::make_async_result_on(executor_,
            [self,
                    request = std::move(request),
                    stop_token]() mutable -> boost::asio::awaitable<support::Expected<void>> {
                if (!self->is_live() || self->hooks_.live_theme == nullptr || self->keybindings_ == nullptr) {
                    // A host that cannot show the URL cannot authorize
                    // anything, and must not wait for a browser.
                    co_return std::unexpected(oauth_unavailable_error());
                }
                if (stop_token.stop_requested()) {
                    co_return std::unexpected(oauth_dismissed_error());
                }
                const auto captured_generation = self->action_generation();
                const std::string request_url = request.authorization_url;
                auto slot = std::make_shared<PromptSlot>(co_await boost::asio::this_coro::executor);
                self->track_prompt_slot(slot);
                const auto weak = self->weak_from_this();
                // The user dismissing the dialog, the host cancelling the
                // flow, and the flow itself finishing all end this prompt;
                // first resolution wins, and each of them restores the editor
                // before resolving so the slot is never left on screen.
                // The prompt slot carries a string; the one answer it can
                // carry is the authorization URL, and this flow only needs to
                // know that the user closed the prompt.
                const auto restore_and_resolve = [weak, slot, request_url](support::Expected<void> outcome) {
                    if (const auto self = weak.lock()) {
                        self->post([self, slot, request_url, outcome = std::move(outcome)]() mutable {
                            self->presenter_->restore_prompt_slot();
                            if (outcome) {
                                slot->resolve(support::Expected<std::string>{request_url});
                            } else {
                                slot->resolve(support::Expected<std::string>{std::unexpected(outcome.error())});
                            }
                        });
                    }
                };
                std::optional<std::stop_callback<std::function<void()>>> cancelled_on_stop;
                cancelled_on_stop.emplace(stop_token,
                        [restore_and_resolve] { restore_and_resolve(std::unexpected(oauth_dismissed_error())); });
                auto dialog = std::make_shared<LoginDialogComponent>(
                        self->hooks_.live_theme(),
                        self->keybindings_->get(),
                        "Authorize " + request.server_id,
                        [weak] {
                            if (const auto self = weak.lock()) {
                                self->presenter_->request_render();
                            }
                        },
                        [weak, captured_generation](std::string url) {
                            if (const auto self = weak.lock()) {
                                if (self->action_generation() != captured_generation ||
                                        self->hooks_.open_browser == nullptr) {
                                    return;
                                }
                                self->hooks_.open_browser(captured_generation, std::move(url));
                            }
                        },
                        [restore_and_resolve] { restore_and_resolve(std::unexpected(oauth_dismissed_error())); });
                self->track_oauth_dialog(dialog);
                self->presenter_->replace_prompt_slot(dialog);
                // The URL is the whole of the presentation: a browser is
                // opened best-effort and the link stays on screen for a user
                // whose browser did not open. There is no graphical OAuth
                // progress surface (issue #849 non-goals).
                dialog->show_auth(
                        request.authorization_url, "Complete the authorization in your browser, then return to pike.");

                boost::system::error_code error;
                auto dismissed = co_await slot->channel.async_receive(
                        boost::asio::redirect_error(boost::asio::use_awaitable, error));
                cancelled_on_stop.reset();
                self->untrack_prompt_slot(slot);
                self->untrack_oauth_dialog(dialog);
                if (self->closed_ || error || !dismissed) {
                    co_return std::unexpected(oauth_dismissed_error());
                }
                co_return support::Expected<void>{};
            });
}

void SessionFlowController::report_mcp_auth_outcome(const McpOAuthOutcome& outcome) {
    // Posted, not applied: the prompt slot and the presenter belong to the
    // controller's executor, and the MCP Host's flow may report from its own
    // domain.
    const auto message = oauth_status_message(outcome);
    post([this, message] {
        if (!closed_) {
            presenter_->show_status(message);
        }
    });
}

boost::asio::awaitable<void> SessionFlowController::handle_mcp_auth(std::string server_id) {
    auto* session = current_session();
    if (session == nullptr || !is_live()) co_return;
    const auto captured_generation = action_generation();
    auto outcome = co_await support::detail::await_async_result(session->mcp_authorize(server_id));
    // A late authorization admitted before a Session replacement must not
    // report against the replacement (ADR 0040).
    if (closed_ || captured_generation != action_generation()) co_return;
    presenter_->restore_prompt_slot();
    if (outcome) {
        presenter_->show_status(oauth_status_message(*outcome));
        co_return;
    }
    if (outcome.error().code == support::ErrorCode::Cancelled) {
        presenter_->show_status("Authorization dismissed for Upstream MCP Server " + server_id);
        co_return;
    }
    presenter_->show_error(
            "Could not authorize Upstream MCP Server " + server_id + ": " + combined_error_text(outcome.error()));
}

} // namespace cch::coding_agent::tui

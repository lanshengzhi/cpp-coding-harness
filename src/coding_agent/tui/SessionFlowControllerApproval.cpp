// The Native TUI's call-approval prompt for an `approval: "ask"` Upstream MCP
// Server (issue #843, spec #833 story 22). Split out of
// SessionFlowController.cpp beside the trust flows it mirrors.

#include "SessionFlowController.hpp"

#include "coding_agent/tui/McpToolApprovalPrompt.hpp"
#include "coding_agent/tui/PromptSlot.hpp"
#include "coding_agent/tui/SharedKeybindings.hpp"
#include "coding_agent/tui/Theme.hpp"
#include "support/AsyncResultBridge.hpp"

#include <cch/support/Error.hpp>

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

[[nodiscard]] support::Error approval_cancelled_error() {
    return support::make_error(support::ErrorCode::Cancelled, "Call approval dismissed");
}

[[nodiscard]] support::Error approval_unavailable_error() {
    return support::make_error(
            support::ErrorCode::Cancelled, "Call approval prompt is not available", "the Native TUI is not live");
}

} // namespace

support::AsyncResult<coding_agent::McpToolApprovalAnswer> SessionFlowController::ask_mcp_tool_approval(
        coding_agent::McpToolApprovalRequest request, std::stop_token stop_token) {
    auto self = shared_from_this();
    // The question is presented on this controller's own executor — the
    // presenter and its prompt slot are executor-confined — and the terminal
    // value travels back to the Agent's domain through the operation's own
    // completion.
    return support::detail::make_async_result_on(executor_,
            [self, request = std::move(request), stop_token]() mutable
                    -> boost::asio::awaitable<support::Expected<coding_agent::McpToolApprovalAnswer>> {
                if (!self->is_live() || self->hooks_.live_theme == nullptr || self->keybindings_ == nullptr) {
                    // A host that cannot present a question cannot obtain
                    // consent, and the failure is what the policy refuses on.
                    co_return std::unexpected(approval_unavailable_error());
                }
                if (stop_token.stop_requested()) {
                    co_return std::unexpected(approval_cancelled_error());
                }
                auto slot = std::make_shared<PromptSlot>(co_await boost::asio::this_coro::executor);
                self->track_prompt_slot(slot);
                // The answers are delivered from the input thread, so each one
                // is marshalled onto the host executor before it restores the
                // editor and resolves the slot: first resolution wins, and a
                // dismissal is never consent.
                const auto weak = self->weak_from_this();
                const auto restore_and_resolve = [weak, slot](support::Expected<std::string> outcome) {
                    if (const auto self = weak.lock()) {
                        self->post([self, slot, outcome = std::move(outcome)]() mutable {
                            self->presenter_->restore_prompt_slot();
                            slot->resolve(std::move(outcome));
                        });
                    }
                };
                // A run that is interrupted while the question is on screen
                // must not wait for an answer that the interrupt already
                // cancelled: the run's own stop token dismisses the prompt, and
                // the call is refused like any other dismissal. Stopping the
                // watcher happens on this executor, never inside its callback.
                std::optional<std::stop_callback<std::function<void()>>> cancelled_on_stop;
                cancelled_on_stop.emplace(stop_token,
                        [restore_and_resolve] { restore_and_resolve(std::unexpected(approval_cancelled_error())); });
                auto prompt = std::make_shared<McpToolApprovalPromptComponent>(
                        self->hooks_.live_theme(),
                        self->keybindings_->get(),
                        std::move(request),
                        [restore_and_resolve] {
                            restore_and_resolve(support::Expected<std::string>{
                                    std::string{McpToolApprovalPromptComponent::allow_value()}});
                        },
                        [restore_and_resolve] {
                            restore_and_resolve(support::Expected<std::string>{
                                    std::string{McpToolApprovalPromptComponent::deny_value()}});
                        },
                        [restore_and_resolve] { restore_and_resolve(std::unexpected(approval_cancelled_error())); });
                self->presenter_->replace_prompt_slot(std::move(prompt));

                boost::system::error_code error;
                auto chosen = co_await slot->channel.async_receive(
                        boost::asio::redirect_error(boost::asio::use_awaitable, error));
                cancelled_on_stop.reset();
                self->untrack_prompt_slot(slot);
                if (self->closed_ || error || !chosen) {
                    co_return std::unexpected(approval_cancelled_error());
                }
                if (*chosen == McpToolApprovalPromptComponent::allow_value()) {
                    co_return coding_agent::McpToolApprovalAnswer::Allowed;
                }
                if (*chosen == McpToolApprovalPromptComponent::deny_value()) {
                    co_return coding_agent::McpToolApprovalAnswer::Declined;
                }
                co_return std::unexpected(approval_cancelled_error());
            });
}

} // namespace cch::coding_agent::tui

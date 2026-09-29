#include "coding_agent/tui/McpFlowController.hpp"

#include "coding_agent/tui/SharedKeybindings.hpp"
#include "coding_agent/tui/Theme.hpp"
#include "coding_agent/AgentSession.hpp"

#include <cch/coding_agent/McpElicitation.hpp>
#include <cch/support/Error.hpp>

#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace cch::coding_agent::tui {
McpFlowController::McpFlowController(boost::asio::any_io_executor executor,
        ModalPresenter& presenter,
        std::weak_ptr<void> host_lifetime,
        McpFlowHostHooks hooks,
        std::shared_ptr<SharedKeybindings> keybindings)
    : executor_(std::move(executor)), presenter_(&presenter), host_lifetime_(std::move(host_lifetime)),
      hooks_(std::move(hooks)), keybindings_(std::move(keybindings)) {}

std::string McpFlowController::showing() const { return showing_; }

void McpFlowController::show_pending() {
    if (!hooks_.post_on_executor) {
        return;
    }
    auto self = shared_from_this();
    hooks_.post_on_executor([self] { self->show_pending_on_host(); });
}

void McpFlowController::show_pending_on_host() {
    if (!host_lifetime_.lock() || dialog_ != nullptr) {
        return; // one question at a time: the prompt slot holds one component
    }
    auto* session = hooks_.current_session ? hooks_.current_session() : nullptr;
    if (session == nullptr) {
        return;
    }
    const auto pending = session->pending_mcp_elicitations();
    if (pending.empty()) {
        return;
    }
    // The first question is the one the user is asked; a form-mode question
    // is not presented by this build and the session already refused it, so
    // anything that reaches here is a question it can put on screen.
    show(pending.front());
}

void McpFlowController::show(const McpPendingElicitation& request) {
    auto* session = hooks_.current_session ? hooks_.current_session() : nullptr;
    if (session == nullptr || !hooks_.live_theme || !hooks_.action_generation) {
        return;
    }
    const auto generation = hooks_.action_generation();
    auto self = shared_from_this();
    auto weak = weak_from_this();
    auto dialog = std::make_shared<McpElicitationDialog>(
            hooks_.live_theme(),
            keybindings_ ? keybindings_->get() : nullptr,
            McpUrlElicitationView{.request = request},
            [weak](McpElicitationAction action, std::string elicitation_id) {
                const auto owner = weak.lock();
                if (owner == nullptr) {
                    return; // a retired host answers nothing
                }
                owner->on_answer(action, std::move(elicitation_id));
            },
            [weak, generation](std::string url) {
                const auto owner = weak.lock();
                if (owner == nullptr || !owner->hooks_.open_browser) {
                    return;
                }
                // The request carries the generation that admitted the
                // dialog, so a retired session's late click cannot open a
                // browser (ADR 0040's action seam).
                owner->hooks_.open_browser(generation, std::move(url));
            },
            [presenter = presenter_]() { presenter->request_render(); });
    dialog_ = std::move(dialog);
    showing_ = request.elicitation_id;
    presenter_->replace_prompt_slot(dialog_);
    presenter_->request_render();
}

void McpFlowController::on_answer(McpElicitationAction action, std::string elicitation_id) {
    auto* session = hooks_.current_session ? hooks_.current_session() : nullptr;
    // The dialog is retired before the answer is sent: the session's answer
    // path may complete the suspended call inline, and a dialog still on
    // screen for a call that has already continued would be a second live
    // question for one call.
    close();
    if (session == nullptr) {
        return;
    }
    (void)session->answer_mcp_elicitation(McpElicitationAnswer{
            .elicitation_id = std::move(elicitation_id),
            .action = action,
    });
    // The next question, if the Upstreams blocked more than one, is asked
    // now rather than at the next frame.
    show_pending_on_host();
}

void McpFlowController::close() {
    // The dialog is withdrawn, not answered: a question being torn down with
    // its session has no answer to give, and a dialog still holding its
    // answer sink would be able to answer a call that is gone.
    if (dialog_) {
        dialog_->withdraw();
    }
    dialog_.reset();
    showing_.clear();
    if (presenter_ != nullptr) {
        presenter_->restore_prompt_slot();
    }
}

} // namespace cch::coding_agent::tui

#pragma once

#include "coding_agent/tui/ModalPresenter.hpp"
#include "coding_agent/tui/McpElicitationDialog.hpp"
#include "coding_agent/tui/McpElicitationFormDialog.hpp"
#include "coding_agent/tui/McpElicitationPrompt.hpp"

#include <cch/coding_agent/McpElicitation.hpp>
#include <cch/support/Error.hpp>

#include <boost/asio/any_io_executor.hpp>

#include <cstddef>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace cch::coding_agent {
class AgentSession;
} // namespace cch::coding_agent

namespace cch::coding_agent::tui {

class LiveTheme;
class SharedKeybindings;

/// The host seams the Pending Elicitation flow needs. The controller owns
/// when a question is shown and what the answer is; the host supplies the
/// session, the palette, and the one environment action (opening the Upstream
/// page) so a retired session's late dialog cannot launch a browser.
struct McpFlowHostHooks {
    /// Marshal one action onto the host executor. Dropped once the host
    /// stops running.
    std::move_only_function<void(std::move_only_function<void()>)> post_on_executor{nullptr};
    /// Resolve the session at execution time. The pointer is borrowed; the
    /// controller re-resolves it after every suspension.
    std::move_only_function<AgentSession*()> current_session{nullptr};
    /// Resolve the live component palette when a dialog opens.
    std::move_only_function<const LiveTheme&()> live_theme{nullptr};
    /// Return the action generation that admitted the dialog.
    std::move_only_function<std::size_t()> action_generation{nullptr};
    /// Deliver one browser-open request with the generation that admitted it.
    std::move_only_function<void(std::size_t, std::string)> open_browser{nullptr};
};

/// The Native TUI Pending Elicitation flow (issue #845; spec #833 story 27).
///
/// The dialog is a **projection**: it renders a `cch_coding_agent`
/// `McpPendingElicitation` row and answers through the session's own API, so
/// the frontend never names an MCP type and never learns a protocol detail
/// (ADR 0065). The controller shows at most one question at a time and
/// remembers which it is showing, because two Upstreams blocked at once are
/// two questions and the prompt slot holds one component.
class McpFlowController final : public std::enable_shared_from_this<McpFlowController> {
public:
    McpFlowController(boost::asio::any_io_executor executor,
            ModalPresenter& presenter,
            std::weak_ptr<void> host_lifetime,
            McpFlowHostHooks hooks,
            std::shared_ptr<SharedKeybindings> keybindings);
    McpFlowController(const McpFlowController&) = delete;
    McpFlowController& operator=(const McpFlowController&) = delete;
    McpFlowController(McpFlowController&&) = delete;
    McpFlowController& operator=(McpFlowController&&) = delete;
    ~McpFlowController() = default;

    /// Show the next question the session is blocked on, if any and if none
    /// is already up. Any-thread entry: the call marshals onto the host
    /// executor. A question that is no longer pending by the time the
    /// controller reaches it is skipped, which is what makes a withdrawn
    /// question (its wait stopped, or its session closed) harmless.
    void show_pending();

    /// Withdraw the dialog and restore the editor. Idempotent, and called on
    /// host Close so a retired session leaves no dialog behind.
    void close();

    /// The elicitation the dialog on screen is asking about, empty when none
    /// is up. The read model a test asserts against.
    [[nodiscard]] std::string showing() const;

private:
    void show_pending_on_host();
    /// The URL-mode question: the address, an open-browser action, and the
    /// three answers.
    void show_url(const McpPendingElicitation& request);
    /// The form-mode question: the fields the Upstream's schema declares,
    /// their validation, and the same three answers (issue #846).
    void show_form(const McpPendingElicitation& request);
    /// One user answer, delivered through the session's own API. The dialog
    /// is retired first and the next question is asked immediately, so two
    /// Upstreams blocked at once are two questions in sequence rather than one
    /// dialog that hides the other. The values are the form mode's half of the
    /// answer and are empty for URL mode.
    void on_answer(McpElicitationAction action, std::string elicitation_id, McpElicitationFormValues values);

    boost::asio::any_io_executor executor_;
    ModalPresenter* presenter_; // kept alive by host_lifetime_ across flows
    std::weak_ptr<void> host_lifetime_;
    McpFlowHostHooks hooks_;
    std::shared_ptr<SharedKeybindings> keybindings_;
    /// The live question in whichever mode asked it, so a second
    /// `show_pending` is a no-op and a question already on screen is not
    /// re-opened. Executor-confined; see close().
    std::shared_ptr<McpElicitationPrompt> dialog_{nullptr};
    std::string showing_{};
};

} // namespace cch::coding_agent::tui

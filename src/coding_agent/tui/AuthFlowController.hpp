#pragma once

#include "coding_agent/tui/LoginDialog.hpp"
#include "coding_agent/tui/ModalPresenter.hpp"
#include "coding_agent/tui/OAuthSelector.hpp"

#include <cch/ai/Auth.hpp>
#include <cch/ai/Model.hpp>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>

#include <cstddef>
#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace cch::coding_agent {
class AgentSession;
} // namespace cch::coding_agent

namespace cch::coding_agent::tui {

class LiveTheme;
class SharedKeybindings;
struct PromptSlot;

/// The host seams required by the Native TUI authentication flows. The
/// controller owns provider-selection and Auth Interaction orchestration;
/// the host supplies the current session, executor marshalling, and the one
/// environment action (opening an OAuth URL).
struct AuthFlowHostHooks {
    /// Marshal one input-thread action onto the host executor. The action is
    /// dropped once the host stops running.
    std::move_only_function<void(std::move_only_function<void()>)> post_on_executor{nullptr};
    /// Spawn one detached flow coroutine on the host executor. A frame
    /// failure is reported through the supplied label.
    std::move_only_function<void(
        std::move_only_function<boost::asio::awaitable<void>()>,
        std::string failure_label)>
        spawn_flow{nullptr};
    /// Resolve the session at execution time. The returned pointer is
    /// borrowed; the controller re-resolves it after every suspension, and
    /// the host retains retired sessions until admitted flows quiesce.
    std::move_only_function<AgentSession*()> current_session{nullptr};
    /// Resolve the live component palette when a dialog or selector opens.
    std::move_only_function<const LiveTheme&()> live_theme{nullptr};
    /// Return the action generation that admitted a dialog.
    std::move_only_function<std::size_t()> action_generation{nullptr};
    /// Deliver one browser-open request with the generation that admitted it.
    /// The host rejects requests from retired session generations.
    std::move_only_function<void(std::size_t, std::string)> open_browser{nullptr};
};

/// Native TUI authentication flow controller (pi `interactive-mode.ts`
/// login/logout flows, #504). It owns provider and authentication-method
/// selection, OAuth/API-key Auth Interaction prompts, post-login model
/// selection, and credential revocation. Presentation reaches the terminal
/// only through ModalPresenter; session access and host operations arrive
/// through AuthFlowHostHooks.
class AuthFlowController final : public std::enable_shared_from_this<AuthFlowController> {
public:
    AuthFlowController(
        boost::asio::any_io_executor executor,
        ModalPresenter& presenter,
        std::weak_ptr<void> host_lifetime,
        AuthFlowHostHooks hooks,
        std::shared_ptr<SharedKeybindings> keybindings);
    AuthFlowController(AuthFlowController&&) = delete;
    AuthFlowController& operator=(AuthFlowController&&) = delete;
    ~AuthFlowController() = default;
    AuthFlowController(const AuthFlowController&) = delete;
    AuthFlowController& operator=(const AuthFlowController&) = delete;

    /// pi `handleLoginCommand`: `/login [provider]`. Any-thread entry.
    void open_login(std::string provider_ref);
    /// pi `showOAuthSelector("logout")`: `/logout`. Any-thread entry.
    void open_logout();
    /// Cancel admitted dialog/prompt work before the host restores its
    /// terminal. Executor-confined like the presenter.
    void close();

private:
    [[nodiscard]] boost::asio::awaitable<void> handle_login_command(
        std::string provider_ref);
    void show_login_auth_type_selector(
        std::optional<std::vector<AuthSelectorProvider>> provider_options);
    void show_login_provider_selector(
        std::optional<AuthSelectorType> filter,
        std::string initial_search);
    [[nodiscard]] boost::asio::awaitable<void> start_provider_login(
        AuthSelectorProvider option);
    void show_ambient_auth_dialog(const AuthSelectorProvider& option);
    [[nodiscard]] boost::asio::awaitable<void> run_login_dialog(
        std::string provider_id,
        std::string provider_name,
        ai::AuthType type);
    [[nodiscard]] boost::asio::awaitable<void> complete_provider_authentication(
        std::string provider_id,
        std::string provider_name,
        ai::AuthType type,
        ai::Model previous_model);
    [[nodiscard]] boost::asio::awaitable<support::Expected<std::string>> show_auth_select(
        std::shared_ptr<class LoginDialogComponent> dialog,
        ai::AuthPromptSelect select,
        std::optional<std::stop_token> per_prompt);
    [[nodiscard]] boost::asio::awaitable<support::Expected<std::string>> show_auth_prompt(
        std::shared_ptr<class LoginDialogComponent> dialog,
        ai::AuthPrompt prompt);
    void notify_auth_dialog(
        LoginDialogComponent& dialog,
        const ai::AuthEvent& event);
    [[nodiscard]] boost::asio::awaitable<void> run_logout();
    [[nodiscard]] boost::asio::awaitable<void> run_logout_provider(
        AuthSelectorProvider option);

    [[nodiscard]] bool is_live();
    [[nodiscard]] AgentSession* current_session();
    void spawn(
        std::move_only_function<boost::asio::awaitable<void>()> start,
        std::string failure_label);
    [[nodiscard]] std::size_t action_generation();
    [[nodiscard]] OpenBrowserSink open_browser_hook();
    [[nodiscard]] LoginDialogActionSink dialog_invalidate_hook();

    boost::asio::any_io_executor executor_;
    ModalPresenter* presenter_; // kept alive by host_lifetime_ across flows.
    std::weak_ptr<void> host_lifetime_;
    AuthFlowHostHooks hooks_;
    std::shared_ptr<SharedKeybindings> keybindings_;
    /// Every live login dialog, tracked so host Close reaches each admitted
    /// flow's dialog (a second `/login` can admit a concurrent flow that
    /// replaces the slot). Executor-confined; see close().
    std::vector<std::shared_ptr<LoginDialogComponent>> active_dialogs_;
    /// Every live AuthPrompt slot (a select-type prompt swaps a selector
    /// into the slot per flow). Executor-confined; resolved by close().
    std::vector<std::shared_ptr<PromptSlot>> active_prompt_slots_;
    /// Close admission from any thread (open_login/open_logout read it
    /// outside the executor); once set it never clears.
    std::atomic<bool> closed_{false};

    void track_dialog(std::shared_ptr<LoginDialogComponent> dialog);
    void untrack_dialog(const std::shared_ptr<LoginDialogComponent>& dialog);
    void track_prompt_slot(std::shared_ptr<PromptSlot> slot);
    void untrack_prompt_slot(const std::shared_ptr<PromptSlot>& slot);
};

} // namespace cch::coding_agent::tui

#pragma once

#include "coding_agent/AgentSession.hpp"
#include "coding_agent/runtime/AgentSessionCreationRequest.hpp"
#include "coding_agent/runtime/SessionFactory.hpp"
#include "coding_agent/tui/ModalPresenter.hpp"

#include <cch/coding_agent/ProjectResources.hpp>
#include <cch/coding_agent/ProjectTrust.hpp>
#include <cch/coding_agent/Settings.hpp>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace cch::coding_agent::tui {

class LiveTheme;
class SharedKeybindings;
struct PromptSlot;

/// Host operations used by the Native TUI session flows. SessionFlowController
/// owns selector, trust, reload, compaction, and session-replacement
/// orchestration; the host supplies session binding and non-modal view updates
/// without exposing a Terminal or InteractiveView to the controller.
struct SessionFlowHostHooks {
    /// Whether the host is running with a composed view. Boot trust uses this
    /// before an Agent Session exists.
    std::move_only_function<bool()> is_live{nullptr};
    /// Marshal one input-thread action onto the host executor. The action is
    /// dropped once the host stops running.
    std::move_only_function<void(std::move_only_function<void()>)> post_on_executor{nullptr};
    /// Spawn one detached flow coroutine on the host executor.
    std::move_only_function<void(
        std::move_only_function<boost::asio::awaitable<void>()>,
        std::string failure_label)>
        spawn_flow{nullptr};
    /// Resolve the session at execution time. The returned pointer is
    /// borrowed; flows re-resolve it after every suspension, and the host
    /// retains retired sessions until admitted flows quiesce.
    std::move_only_function<AgentSession*()> current_session{nullptr};
    /// Resolve the live component palette when a selector opens.
    std::move_only_function<const LiveTheme&()> live_theme{nullptr};
    /// Number of terminal rows used by the tree selector layout.
    std::move_only_function<std::size_t()> terminal_rows{nullptr};
    /// Return the composition-owned capability collection for one current
    /// Session workspace. Trust detection never creates or widens a root.
    std::move_only_function<ProjectResourceFileSystems(std::filesystem::path)> project_resource_filesystems{nullptr};
    /// Generation that admitted the current application action.
    std::move_only_function<std::size_t()> action_generation{nullptr};

    /// Build an in-session replacement request from the host's CLI-owned
    /// facts and the target workspace/session intent.
    std::move_only_function<runtime::AgentSessionCreationRequest(
        std::filesystem::path,
        SessionTarget)>
        make_session_request{nullptr};
    /// Ask the composition host to create a replacement Agent Session.
    std::move_only_function<support::Expected<CreateAgentSessionResult>(
        std::size_t,
        runtime::AgentSessionCreationRequest)>
        request_session_replacement{nullptr};
    /// Install a created replacement and rebind the Native TUI.
    std::move_only_function<support::ExpectedVoid(std::unique_ptr<AgentSession>)>
        replace_session{nullptr};

    /// Non-modal presentation operations needed by the session flows. These
    /// are value operations on the host's view, not direct terminal access.
    std::move_only_function<void(std::string)> show_warning{nullptr};
    std::move_only_function<void(std::string)> show_frontend_message{nullptr};
    std::move_only_function<void()> clear_status_indicator{nullptr};
    std::move_only_function<void(std::string)> set_editor_text{nullptr};
    std::move_only_function<std::string()> editor_text{nullptr};
    std::move_only_function<bool(std::string)> copy_to_clipboard{nullptr};
    /// Restore queued Agent input before tree navigation aborts a busy turn.
    std::move_only_function<void()> dequeue_pending_input{nullptr};
    std::move_only_function<void()> rebuild_chat{nullptr};
    /// Apply one successful `/reload` result: re-catalog keybindings,
    /// re-register themes, re-apply settings, rebuild autocomplete, refresh
    /// loaded resources, and surface the models.json diagnostic.
    std::move_only_function<support::ExpectedVoid(AgentSessionReloadResult)> apply_reload_result{nullptr};
    /// Mark the manual compaction as admitted or completed so host shutdown
    /// waits for it exactly like prompt/User Bash work.
    std::move_only_function<void(bool)> set_compaction_active{nullptr};
    /// Complete a close request that was waiting for manual compaction.
    std::move_only_function<void()> signal_exit{nullptr};
    /// Request application exit from selector-owned delete actions.
    std::move_only_function<void()> request_exit{nullptr};

    /// Report boot trust diagnostics through the composition host's closed
    /// action seam, retaining the generation stamp used by other actions.
    std::move_only_function<void(std::size_t, std::vector<SessionDiagnostic>)>
        report_boot_diagnostics{nullptr};
};

/// Native TUI session flow controller (pi `interactive-mode.ts` session
/// switching/tree/fork/reload/compact/trust flows, #504). It presents every
/// selector through ModalPresenter and keeps session replacement, prompt
/// resolution, resource reload, and close-sensitive compaction orchestration
/// behind SessionFlowHostHooks.
class SessionFlowController final : public std::enable_shared_from_this<SessionFlowController> {
public:
    SessionFlowController(
        boost::asio::any_io_executor executor,
        ModalPresenter& presenter,
        std::weak_ptr<void> host_lifetime,
        SessionFlowHostHooks hooks,
        std::shared_ptr<SharedKeybindings> keybindings,
        coding_agent::SettingsManager* settings_manager);
    SessionFlowController(SessionFlowController&&) = delete;
    SessionFlowController& operator=(SessionFlowController&&) = delete;
    ~SessionFlowController() = default;
    SessionFlowController(const SessionFlowController&) = delete;
    SessionFlowController& operator=(const SessionFlowController&) = delete;

    /// Any-thread entries for the in-session session flows.
    void open_resume();
    void open_fork();
    void open_new();
    void open_tree();
    void open_compact(std::string custom_instructions);
    void open_reload();
    void open_trust();

    /// Cancel admitted selector/flow work before the host restores its
    /// terminal. Executor-confined like the presenter.
    void close();

    /// Boot trust preparation and resolution. The preparation arms pi's
    /// implicit-trust-on-reload behavior when the boot workspace had no
    /// trust-requiring resources.
    [[nodiscard]] boost::asio::awaitable<support::ExpectedVoid> arm_auto_trust_on_reload(
            std::filesystem::path workspace, std::optional<bool> trust_override);
    [[nodiscard]] boost::asio::awaitable<support::Expected<bool>> resolve_boot_trust(
            std::filesystem::path workspace, std::optional<bool> trust_override);

private:
    [[nodiscard]] boost::asio::awaitable<void> handle_resume_session(
        std::string session_path);
    [[nodiscard]] boost::asio::awaitable<void> handle_new_session();
    void show_user_message_selector();
    [[nodiscard]] boost::asio::awaitable<void> handle_fork_session(
        std::string entry_id);
    [[nodiscard]] boost::asio::awaitable<std::optional<std::filesystem::path>>
    prompt_for_missing_session_cwd(
        std::filesystem::path session_cwd,
        std::filesystem::path fallback_cwd);
    void show_session_selector();
    void show_tree_selector();
    [[nodiscard]] boost::asio::awaitable<void> handle_tree_navigation(
        std::string entry_id);
    void handle_tree_copy(std::optional<std::string> text);
    [[nodiscard]] boost::asio::awaitable<void> handle_compact_command(
        std::string custom_instructions);
    [[nodiscard]] boost::asio::awaitable<void> handle_reload();
    void show_trust_selector();
    [[nodiscard]] boost::asio::awaitable<std::optional<ProjectTrustOption>>
    show_boot_trust_prompt(const std::filesystem::path& workspace);
    [[nodiscard]] boost::asio::awaitable<void> run_trust_selector();
    [[nodiscard]] boost::asio::awaitable<support::Expected<ProjectResourceDetectionResult>>
    detect_project_resources_for(const std::filesystem::path& workspace);

    [[nodiscard]] bool is_live();
    [[nodiscard]] AgentSession* current_session();
    [[nodiscard]] std::size_t action_generation();
    [[nodiscard]] runtime::AgentSessionCreationRequest make_session_request(
        std::filesystem::path workspace,
        SessionTarget target);
    void post(std::move_only_function<void()> action);
    void spawn(
        std::move_only_function<boost::asio::awaitable<void>()> start,
        std::string failure_label);
    [[nodiscard]] support::Expected<coding_agent::CreateAgentSessionResult>
    request_session_replacement(runtime::AgentSessionCreationRequest request);
    [[nodiscard]] support::ExpectedVoid replace_session(
        std::unique_ptr<AgentSession> session);
    [[nodiscard]] boost::asio::awaitable<support::Expected<bool>> maybe_save_implicit_project_trust_after_reload();

    boost::asio::any_io_executor executor_;
    ModalPresenter* presenter_; // kept alive by host_lifetime_ across flows.
    std::weak_ptr<void> host_lifetime_;
    SessionFlowHostHooks hooks_;
    std::shared_ptr<SharedKeybindings> keybindings_;
    coding_agent::SettingsManager* settings_manager_; // must outlive every flow.
    /// Every live user-prompt slot (boot-trust, missing-cwd, and in-session
    /// trust prompts), tracked so host Close resolves each admitted flow's
    /// slot and finish() can await quiescence. Executor-confined.
    std::vector<std::shared_ptr<PromptSlot>> active_prompt_slots_;
    std::optional<std::filesystem::path> auto_trust_on_reload_cwd_;
    std::optional<std::filesystem::path> boot_detection_workspace_;
    std::optional<ProjectResourceDetectionResult> boot_detection_;
    /// Close admission from any thread (open_* read it outside the
    /// executor); once set it never clears.
    std::atomic<bool> closed_{false};
    std::stop_source stop_source_;

    void track_prompt_slot(std::shared_ptr<PromptSlot> slot);
    void untrack_prompt_slot(const std::shared_ptr<PromptSlot>& slot);
};

} // namespace cch::coding_agent::tui

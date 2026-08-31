#pragma once

// The streamlined Native TUI orchestration core (#501 spec; consolidation
// #506): the former InteractiveState god class, reduced to wiring the
// Terminal/Tui, InteractiveView, SlashCommandRouter, the modal flow
// controllers (Model/Auth/Session/Settings), the SessionUiBinding adapter,
// and the SuspendController together, and to owning the run's lifecycle
// (start, deferred boot, Close) plus the ModalPresenter seam those
// collaborators present through.
//
// The implementation is split into cohesive units (the repository's split
// implementation pattern, e.g. SessionFlowControllerTrust.cpp):
// - InteractiveEngine.cpp:        composition and lifecycle.
// - InteractiveEngineWiring.cpp:  collaborator factories and flow spawning.
// - InteractiveEngineHost.cpp:    slash-command effects, session replacement,
//                                 and closed action delivery.
// - InteractiveMode.cpp:          the public entries plus the input-event
//                                 pumping members (view-action admission and
//                                 dispatch, slash routing, submissions,
//                                 interrupts, prompt/User Bash lifecycle).
//
// Repository-private `cch_coding_agent` implementation header: not part of
// an Owner Interface, not installed, never exported.

#include "coding_agent/SessionTarget.hpp"
#include "coding_agent/runtime/UserBash.hpp"
#include "coding_agent/tui/InteractiveMode.hpp"
#include "coding_agent/tui/InteractiveSessionRun.hpp"
#include "coding_agent/tui/InteractiveStartup.hpp"
#include "coding_agent/tui/InteractiveViewActions.hpp"
#include "coding_agent/tui/ModalPresenter.hpp"
#include "coding_agent/tui/SlashCommandRouter.hpp"

#include <cch/coding_agent/ProjectResources.hpp>
#include <cch/coding_agent/Settings.hpp>
#include <cch/tui/Tui.hpp>
#include <cch/support/Error.hpp>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/steady_timer.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cch::coding_agent::tui {

class AuthFlowController;
class InteractiveView;
class ModelFlowController;
class SessionFlowController;
class SessionUiBinding;
class SettingsFlowController;
class SharedKeybindings;
class SuspendController;
class ThemeController;

} // namespace cch::coding_agent::tui

namespace cch::tui {
class AutocompleteProvider;
} // namespace cch::tui

namespace cch::coding_agent::tui {

/// The pi main-screen orchestration core (#506). One instance per
/// interactive run; the host drives `start` → (`boot_session` on the
/// deferred-boot entry) → exit wait → `finish`. Executor-confined except
/// where noted; collaborators capture it weakly and its Close awaits their
/// quiescence (ADR 0040).
class InteractiveEngine final
    : public std::enable_shared_from_this<InteractiveEngine>,
      public ModalPresenter {
public:
    /// The terminal must outlive the run.
    InteractiveEngine(
        cch::tui::Terminal& terminal,
        boost::asio::any_io_executor executor);
    InteractiveEngine(InteractiveEngine&&) = delete;
    InteractiveEngine& operator=(InteractiveEngine&&) = delete;
    ~InteractiveEngine() override;
    InteractiveEngine(const InteractiveEngine&) = delete;
    InteractiveEngine& operator=(const InteractiveEngine&) = delete;

    /// Load startup resources, compose and attach the view, create the flow
    /// controllers and session binding, start the TUI, and (outside the
    /// deferred-boot path) bind the session and submit the initial prompt.
    [[nodiscard]] support::ExpectedVoid start(InteractiveSessionRun run);

    /// pi main.ts `createAgentSessionRuntime` + `resolveProjectTrusted`:
    /// the deferred boot of the interactive host. Resolves boot trust
    /// (prompt overlay when a trust-requiring resource exists and no
    /// override is set), creates the boot session through the config's
    /// `boot_request`/`action_sink` with the decided trust, then binds
    /// it (subscribe, initialize view, render, initial prompt).
    [[nodiscard]] boost::asio::awaitable<support::ExpectedVoid> boot_session();

    /// Detect the canonical project resources for the bound Session and append
    /// the Native TUI warning when the Session is untrusted. The detection is
    /// awaited on the serialized runtime path so a replaced Session cannot
    /// receive a stale warning.
    [[nodiscard]] boost::asio::awaitable<support::Expected<bool>> append_project_trust_warning_if_needed();

    /// The exit wait: released by `signal_exit` once the run may tear down.
    [[nodiscard]] boost::asio::steady_timer& exit_wait() {
        return exit_wait_;
    }

    /// Final application Close (ADR 0040): cancel the extracted modal/session
    /// flows, stop admission, retire the action generation, and await every
    /// admitted detached flow to reach a terminal outcome before terminal
    /// restoration. The Close above drives dialogs, selectors, and auth
    /// interactions to a terminal outcome; each flow's host-lifetime capture
    /// keeps the engine (the production ModalPresenter) alive until this wait
    /// returns, so no admitted coroutine can resume against a stopped
    /// presenter or a destroyed executor. The current Session's exit path
    /// already waited for prompt/User Bash/compaction settle, so its Close
    /// finalizes synchronously here.
    [[nodiscard]] boost::asio::awaitable<support::ExpectedVoid> finish();

private:
    // ── Submission kinds (folded from the deleted InteractionPolicy) ──────

    enum class SubmissionOrigin { FocusedEditor, InitialPrompt };

    enum class InterruptRoute {
        AbortAgentRun,
        CancelUserBash,
        ClearPendingBash,
        None,
    };

    // ── Startup resources and view composition ───────────────────────────

    /// `/reload` keybinding re-catalog (pi `KeybindingsManager.reload()`):
    /// re-run the assembled catalog load with the same action list and swap
    /// the shared slot + editor (ADR 0035). Diagnostics render like startup.
    [[nodiscard]] support::ExpectedVoid re_catalog_keybindings();

    [[nodiscard]] support::Expected<InteractiveStartupDiagnostics> load_startup_resources(
        const InteractiveSessionRun& run);

    [[nodiscard]] std::unique_ptr<InteractiveView> make_interactive_view(
        std::weak_ptr<InteractiveEngine> weak);

    /// The view's user-shell hint: the interactive host always provides a
    /// User Shell to the boot session (`provide_user_shell`), so the boot
    /// path reports it before the session exists.
    [[nodiscard]] bool view_user_shell_available() const;

    /// pi `createBaseAutocompleteProvider`: the combined provider over the
    /// effective commands, prompt templates, and (while the
    /// `enableSkillCommands` setting is enabled) `skill:` commands. Rebuilt
    /// after a setting change exactly like pi's `setupAutocompleteProvider`.
    /// The boot path builds with the boot workspace and no discovered
    /// resources until the session binds (`boot_session` rebuilds it).
    [[nodiscard]] std::unique_ptr<cch::tui::AutocompleteProvider>
    build_autocomplete_provider();

    /// pi `setupAutocompleteProvider` after a settings change: swap the
    /// editor's autocomplete provider for a freshly built one.
    void rebuild_autocomplete_provider();

    /// pi `showLoadedResources`: refresh the loaded-resources block from the
    /// live session (Context sources, skills, templates), the registered
    /// themes, and the per-kind diagnostics (loader read diagnostics plus the
    /// theme discovery diagnostics stashed at boot/reload). Called at view
    /// initialization, after `/reload`, and after session replacement.
    void refresh_loaded_resources();

    void initialize_view(const InteractiveStartupDiagnostics& diagnostics);

    /// Return the capability collection for one current Session workspace.
    /// The boot collection is reused when it matches; a replacement gets a
    /// fresh collection from the same composition helper.
    [[nodiscard]] ProjectResourceFileSystems project_resource_filesystems_for(const std::filesystem::path& workspace);

    [[nodiscard]] support::ExpectedVoid fail_start(const support::Error& error);

    // ── Executor hops and render requests ────────────────────────────────

    void post_invalidate();
    void post_exit();
    void post_render();
    void post_close_overlay();

    /// Post one view-thread action to the executor.
    void post_from_view(std::move_only_function<void(InteractiveEngine&)> action);

    // ── Input event pumping (InteractiveMode.cpp) ────────────────────────

    /// Admit one closed main-screen action to the serialized executor path
    /// (ADR 0040 shape). The interrupt's prompt generation is captured at
    /// admission — matching the pre-seam `post_interrupt` timing — so a fast
    /// session switch cannot retarget it; every other action carries no
    /// generation. Exactly one executor hop, then `dispatch_view_action`.
    void post_view_action(ViewAction action);

    /// Route one admitted view action to its domain behavior on the
    /// serialized path. Dispatch performs the domain call directly (no second
    /// hop), preserving admission order.
    void dispatch_view_action(ViewAction action, std::size_t prompt_generation);

    void dispatch(SubmitAction action);
    void dispatch(ClipboardPasteAction);
    void dispatch(DequeueAction);
    void dispatch(ExitAction);
    void dispatch(CycleModelAction action);
    void dispatch(CycleThinkingAction);
    void dispatch(ToggleThinkingAction);
    void dispatch(SelectModelAction);
    void dispatch(ResumeSessionAction);
    void dispatch(ForkSessionAction);
    void dispatch(NewSessionAction);
    void dispatch(CopyLastMessageAction);
    void dispatch(OpenTreeSelectorAction);
    void dispatch(SuspendAction);
    void dispatch(ExternalEditorAction);

    /// pi `handleOpenExternalEditor`: the extracted flow stops the TUI,
    /// runs the editor over the expanded content, and restores/re-renders on
    /// every path; a terminal lifecycle failure completes the run.
    [[nodiscard]] boost::asio::awaitable<void> handle_open_external_editor();

    // ── Slash routing (router orchestration) ─────────────────────────────

    /// Dynamic prompt-template and skill invocations remain ordinary Agent
    /// Prompt submissions after built-in routing. Built-in names win over
    /// resources with the same spelling, matching the autocomplete collision
    /// rule.
    [[nodiscard]] bool is_dynamic_slash_command(std::string_view command) const;

    /// Route built-in slash commands through the deep SlashCommandRouter. The
    /// router executes in-place commands through one small context seam and
    /// returns modal requests as passive values; this method only binds those
    /// values to the existing Native TUI flows.
    [[nodiscard]] bool dispatch_command(std::string_view text);

    [[nodiscard]] support::ExpectedVoid execute_immediate_slash_command(
        const SlashCommandInvocation& invocation);

    void dispatch_modal_slash_command(SlashCommandInvocation invocation);

    /// pi `handleSessionCommand`: render the Session Info chat block.
    void handle_session_command();
    /// pi `handleNameCommand`: `/name <name>` sanitizes and persists the
    /// `session_info` entry and reports pi's statuses; a bare `/name` shows
    /// the current name or the usage warning.
    void handle_name_command(std::string name);
    /// pi `handleCopyCommand`: copy the last assistant message's text and
    /// report the pi statuses.
    void handle_copy_last_message();
    void show_help_command();
    void open_hotkeys();

    // ── Interrupt admission (pi onEscape precedence, folded from the
    //    deleted InterruptAdmission) ──────────────────────────────────────

    /// The prompt generation captured when an input-thread request is posted.
    [[nodiscard]] std::size_t generation() const noexcept {
        return prompt_generation_.load();
    }

    /// Advances admission state immediately before a new Agent prompt starts.
    void note_prompt_started() noexcept {
        (void)prompt_generation_.fetch_add(1);
        interrupt_requested_generation_.reset();
    }

    /// Invalidates requests captured before the active Agent prompt finished.
    void note_prompt_finished() noexcept {
        (void)prompt_generation_.fetch_add(1);
        interrupt_requested_generation_.reset();
    }

    /// Reports whether the active prompt generation already admitted an abort.
    [[nodiscard]] bool interrupt_requested() const noexcept {
        return interrupt_requested_generation_ == prompt_generation_.load();
    }

    /// Admits a current interrupt request and selects its pi-ordered target:
    /// an active Agent run aborts first, then a running User Bash cancels,
    /// then a pending User Bash submission clears the editor.
    [[nodiscard]] InterruptRoute admit_interrupt(
        std::size_t captured_generation,
        bool pending_bash) noexcept;

    void request_interrupt(
        std::size_t prompt_generation,
        const EditorInterruptRequest& request);

    // ── Submission routing (pi setupEditorSubmitHandler, folded from the
    //    deleted InteractionPolicy) ───────────────────────────────────────

    [[nodiscard]] bool dispatch_user_bash(const std::string& text, SubmissionOrigin origin);

    void submit(
        std::string text,
        InputSubmission submission,
        PromptOptions options = {},
        SubmissionOrigin origin = SubmissionOrigin::FocusedEditor,
        std::optional<std::size_t> editor_revision = std::nullopt);

    void dequeue_pending_input(bool announce);

    void prompt_launch_failed(std::size_t started_generation);
    void prompt_finished(
        std::size_t started_generation,
        support::ExpectedVoid result,
        const std::string& submitted_text);
    void user_bash_finished(
        std::size_t started_generation,
        support::Expected<runtime::UserBashCompletion> result,
        const std::string& recall);

    // ── Overlays and the ModalPresenter seam (#503): the engine is the
    //    production presenter — overlays, prompt-slot swaps, and status text
    //    land on the view/terminal it owns. ───────────────────────────────

    /// Append one bounded presentation error to the chat diagnostic area.
    void append_command_error(const support::Error& error);

    [[nodiscard]] support::ExpectedVoid attach_overlay(
        std::unique_ptr<cch::tui::Overlay> overlay);
    void close_overlay() override;

    void show_overlay(std::unique_ptr<cch::tui::Overlay> overlay) override;
    /// pi `showSelector` editorContainer swap: a modal component replaces
    /// the editor slot.
    void replace_prompt_slot(std::shared_ptr<cch::tui::Component> component) override;
    void restore_prompt_slot() override;
    /// pi `showStatus`: one dim status line in the chat.
    void show_status(std::string text) override;
    /// pi `showError`: one `Error: <text>` chat line.
    void show_error(std::string text) override;
    /// pi `ui.requestRender`: one coalescible re-render request, safe from
    /// any thread (posts the render to the executor).
    void request_render() override;
    /// Mark the frame dirty without rendering immediately.
    void invalidate() override;

    /// Rebuild the chat from the authoritative session snapshot (pi
    /// `rebuildChatFromMessages`): the render settings apply first, the
    /// streaming assistant message re-renders with them, and the
    /// pending-input queue display is restored.
    void rebuild_chat();

    // ── Collaborator wiring (InteractiveEngineWiring.cpp) ────────────────
    // All hooks capture the engine weakly; nothing there extends the
    // engine's lifetime.

    [[nodiscard]] std::shared_ptr<ModelFlowController> make_model_flow_controller();
    [[nodiscard]] std::shared_ptr<AuthFlowController> make_auth_flow_controller();
    [[nodiscard]] std::shared_ptr<SessionFlowController> make_session_flow_controller();
    [[nodiscard]] std::shared_ptr<SessionUiBinding> make_session_ui_binding();
    [[nodiscard]] std::shared_ptr<SettingsFlowController> make_settings_flow_controller();
    [[nodiscard]] std::shared_ptr<SuspendController> make_suspend_controller();

    /// Spawn one detached executor flow; a frame failure becomes a chat
    /// diagnostic (the user-bash precedent; the login flows use this too).
    /// Every admitted flow is counted so `finish()` can await quiescence
    /// (ADR 0040: terminal restoration never races a controller coroutine).
    void spawn_flow(
        std::move_only_function<boost::asio::awaitable<void>()> start,
        std::string failure_label);

    /// One admitted detached flow reached its terminal outcome. The count is
    /// executor-confined (spawn and completion both run on the host
    /// executor); reaching zero releases the `finish()` quiescence wait.
    void flow_finished() noexcept;

    // ── Composition-host seam: command effects, session replacement, and
    //    closed action delivery (InteractiveEngineHost.cpp) ───────────────

    /// The configured clipboard writer (pi `copyToClipboard` platform-tools
    /// path; tests inject a recorder).
    [[nodiscard]] bool write_clipboard_text_sink(std::string text);

    /// Build one in-session session creation request from the CLI-owned facts
    /// (pi `createRuntime` re-resolves the CLI options against the target
    /// cwd).
    [[nodiscard]] runtime::AgentSessionCreationRequest make_session_request(
        std::filesystem::path workspace,
        SessionTarget target) const;

    /// The error a null host returns for `ReplaceSessionAction`.
    [[nodiscard]] static support::Error session_replacement_unavailable_error();

    /// Carry one closed application-level action to the composition host with
    /// the generation that admitted it. A delivery from a retired generation
    /// (the session was replaced or the mode closed) is rejected, so a late
    /// action cannot reach the host; `open_browser_hook()` is the one
    /// captured vector and drops those rejections. A null host applies the
    /// TUI-local platform default for the environment operations. Render
    /// state may coalesce, but this path never drops an admitted action.
    [[nodiscard]] support::Expected<TuiActionResultVariant> deliver_action(
        std::size_t captured_generation,
        TuiActionVariant action);

    /// Create and return a replacement/boot session through the composition
    /// host (pi `createRuntime`); a null host reports it as unavailable.
    [[nodiscard]] support::Expected<coding_agent::CreateAgentSessionResult>
    request_session_replacement(
        std::size_t captured_generation,
        runtime::AgentSessionCreationRequest request);
    [[nodiscard]] boost::asio::awaitable<support::Expected<coding_agent::CreateAgentSessionResult>>
    request_session_replacement_async(std::size_t captured_generation,
            runtime::AgentSessionCreationRequest request,
            std::stop_token stop_token = {});

    /// pi `AgentSessionRuntime.apply` + `rebindCurrentSession` subset: swap
    /// the live session, resubscribe, and rebuild the presentation from the
    /// new session's snapshot. The host-owned view stays in place; the chat
    /// re-renders like pi's `renderCurrentSessionState`. Retires the action
    /// generation so actions admitted by the previous session are rejected.
    ///
    /// Admission design (ADR 0040, issue #466): replacement first closes the
    /// previous current Session synchronously — `close()` stops prompt
    /// admission and requests cancellation of active work (issue #467
    /// semantics) — then installs the new one. The old Session's admitted
    /// work quiesces asynchronously on the shared Runtime root and is never
    /// awaited by the replacement (pi installs the new Session immediately;
    /// the #466 tests assert a fresh prompt starts while the retired run is
    /// still settling). Safety comes from three mechanisms: the old Session
    /// is retained (in `retired_sessions_`, pruned once its Close finalizes)
    /// so a detached flow cannot dereference a destroyed Session; late
    /// completions are retired by the generation stamp
    /// (`prompt_finished`/`user_bash_finished`); and the old Session's
    /// subscriptions are detached before the new one binds.
    [[nodiscard]] support::ExpectedVoid replace_session(
        std::unique_ptr<AgentSession> next);

    /// Retire the current action generation (session replacement or Close):
    /// later deliveries admitted by the retired generation are rejected.
    void retire_action_generation() noexcept { ++action_generation_; }

    /// Retire the current action generation, detach the current Session's
    /// subscriptions, and request Session close (ADR 0040, issue #466): the
    /// previous current Session's prompt admission stops before a replacement
    /// is installed or the mode closes, while its admitted work quiesces
    /// asynchronously on the shared Runtime root.
    void retire_current_session() noexcept;

    /// Reports whether a prompt/User Bash completion was admitted by a
    /// retired Session generation (the Session was replaced or closed). Such
    /// late completions must not mutate or render as the current Session
    /// (issue #466).
    [[nodiscard]] bool generation_retired(
        std::size_t started_generation) const noexcept {
        return started_generation != action_generation_;
    }

    // ── Exit gating ──────────────────────────────────────────────────────

    void render();
    void request_exit();
    void signal_exit();

    // ── State ────────────────────────────────────────────────────────────

    AgentSession* session_; // must outlive this interactive run.
    std::shared_ptr<harness::RuntimeRoot> runtime_root_;
    ProjectResourceFileSystems project_resource_filesystems_;
    /// Owned replacement session (pi `AgentSessionRuntime.switchSession` /
    /// `newSession` / `fork`): the in-session flows recreate the session
    /// through the config factory and keep the replacement alive here. The
    /// initial session stays borrowed from the host.
    std::unique_ptr<AgentSession> owned_session_;
    /// Replaced sessions whose admitted work is still settling stay owned
    /// here (so a detached flow that borrowed one stays safe across its
    /// final await), and are pruned once their Close finalizes; see
    /// replace_session().
    std::vector<std::unique_ptr<AgentSession>> retired_sessions_;
    cch::tui::Terminal& terminal_; // must outlive this interactive run.
    cch::tui::Tui tui_;
    /// pi's mutable shared KeybindingsManager consumption shape (ADR 0035,
    /// #418): every durable view component observes this slot; `/reload`
    /// replaces the current registry so all consumers see the new bindings
    /// live. Selectors take an ephemeral `get()` snapshot.
    std::shared_ptr<SharedKeybindings> keybindings_;
    /// The agent config directory the keybinding catalog was assembled
    /// under, retained for the `/reload` re-catalog.
    std::filesystem::path agent_config_directory_;
    /// Two-scope settings manager (global scope only; the project scope stays
    /// untrusted in the Native TUI). The theme committer and the
    /// scoped-models selector persist through it. Declared before
    /// `theme_controller_` so the controller's committer reference stays
    /// valid through destruction.
    std::optional<coding_agent::SettingsManager> settings_manager_{std::nullopt};
    /// pi `hideThinkingBlock` / `outputPad` render settings, loaded once at
    /// boot from the merged settings and mutated by `app.thinking.toggle` and
    /// the settings selector. The view's chat renders with these values.
    bool hide_thinking_block_{false};
    std::size_t output_pad_{1};
    /// The extracted model, authentication, session, and settings flows,
    /// created once startup resources (live theme, settings manager,
    /// keybindings) exist.
    std::shared_ptr<ModelFlowController> model_flows_;
    std::shared_ptr<AuthFlowController> auth_flows_;
    std::shared_ptr<SessionFlowController> session_flows_;
    /// The settings selector + thinking/render-settings flows (#506).
    std::shared_ptr<SettingsFlowController> settings_flows_;
    /// The session synchronization adapter (#505): owns the Agent Session
    /// event subscriptions, the streaming/retry/compaction event
    /// translation, and the footer data computation. Created with the flow
    /// controllers; `bind()`/`detach()` follow the session lifecycle.
    std::shared_ptr<SessionUiBinding> session_ui_;
    std::unique_ptr<ThemeController> theme_controller_;
    std::unique_ptr<AsyncClipboardReader> clipboard_reader_;
    /// One move-only sink carrying closed application-level actions to the
    /// composition host (ADR 0040); null applies TUI-local platform defaults
    /// for the environment operations.
    TuiActionSink action_sink_{nullptr};
    AsyncSessionReplacementSink async_session_replacement_sink_{nullptr};
    std::optional<std::string> model_fallback_message_;
    /// CLI-owned facts reused for in-session session replacement requests.
    runtime::InteractiveSessionFacts session_facts_;
    /// Boot path (pi main.ts `createRuntime` + `resolveProjectTrust`): the
    /// base creation request the interactive host supplies; the boot creates
    /// the session after the boot trust prompt resolves. Empty outside the
    /// boot entry.
    std::optional<runtime::AgentSessionCreationRequest> boot_request_{std::nullopt};
    /// pi `projectTrustByCwd`: the boot-resolved trust decision for the boot
    /// workspace, reused by in-session session creations in the same
    /// workspace (a session-only choice leaves no store entry).
    std::optional<std::pair<std::filesystem::path, bool>>
        resolved_boot_trust_{std::nullopt};
    /// Action-generation counter for the closed action seam (ADR 0040):
    /// every action is delivered with the generation that admitted it, and
    /// `retire_action_generation()` rejects later deliveries from a retired
    /// session generation. Executor-confined; captured by `open_browser_hook`
    /// at hook creation.
    std::size_t action_generation_{0};

    /// Startup diagnostics stashed by the boot `start()` until the boot
    /// session binds and `initialize_view` renders them (pi
    /// `renderInitialMessages` after the trust prompt).
    InteractiveStartupDiagnostics startup_diagnostics_{};
    /// Theme discovery (parse/collision) diagnostics for the loaded-resources
    /// `[Theme conflicts]` section (pi `getThemes().diagnostics`), stashed at
    /// boot bind and refreshed by `/reload` (#418).
    std::vector<ResourceDiagnostic> loaded_theme_diagnostics_;
    /// Initial prompt stashed by the boot `start()` until the boot session
    /// binds (pi main.ts `initialMessage` submitted after runtime creation).
    std::optional<std::string> initial_prompt_{std::nullopt};
    PromptOptions initial_prompt_options_{};
    boost::asio::any_io_executor executor_;
    boost::asio::steady_timer exit_wait_;
    /// Detached-flow quiescence (ADR 0040): the number of admitted
    /// controller flows still in flight. `finish()` awaits `flows_settled_`
    /// until this reaches zero so terminal restoration never races an
    /// admitted coroutine. Executor-confined; see spawn_flow().
    std::size_t in_flight_flows_{0};
    boost::asio::steady_timer flows_settled_;
    /// SIGTSTP/SIGCONT suspend flow (pi `handleCtrlZ`); created at start.
    std::shared_ptr<SuspendController> suspend_controller_;
    InteractiveView* view_{nullptr}; // aliases the child owned by tui_.
    cch::tui::Overlay* active_overlay_{nullptr}; // aliases an overlay owned by tui_.
    SlashCommandRouter slash_command_router_;
    std::atomic<bool> running_{false};
    std::stop_source stop_source_;
    std::atomic<bool> prompt_active_{false};
    std::atomic<bool> user_bash_active_{false};
    /// A manual /compact flow was admitted and has not returned yet. Exit
    /// defers on it exactly like prompt/User Bash work: the Session Close
    /// requested by request_exit() finalizes only after the compaction
    /// settles, and tearing the loop down earlier would destroy Session
    /// resources the compaction still uses (issue #467, ADR 0040).
    /// Executor-confined like the flows that set and clear it.
    std::atomic<bool> compaction_active_{false};
    /// pi `lastEscapeTime`: the double-escape window base (500 ms, empty
    /// editor, `doubleEscapeAction` default "tree"). Executor-confined.
    std::chrono::steady_clock::time_point last_escape_time_{};
    /// Render-request coalescing for post_invalidate()/post_render(): at
    /// most one queued handler each, so a live animation ticking from the
    /// Loader's detached timer thread (~80 ms) cannot pile duplicate posts
    /// onto a congested loop faster than it drains them (issue #553; the
    /// coalescible render request of InteractiveView.hpp and ADR 0035's
    /// render coalescing). Set off-executor, so atomic.
    std::atomic<bool> invalidate_posted_{false};
    std::atomic<bool> render_posted_{false};
    // Prompt-generation staleness for interrupt requests (pi onEscape
    // routing; the deleted InterruptAdmission's generation). The generation
    // is read from the input thread at post time, so it stays atomic; the
    // admitted-generation marker is executor-confined.
    std::atomic<std::size_t> prompt_generation_{0};
    std::optional<std::size_t> interrupt_requested_generation_;
    // Suppresses a submission already decoded from Bash text cleared by an
    // earlier key-time interrupt decision.
    std::optional<std::size_t> cleared_editor_revision_;
    bool tui_started_{false};
    bool exit_requested_{false};
    bool clipboard_read_active_{false};
    std::optional<support::ExpectedVoid> completion_result_;
};

} // namespace cch::coding_agent::tui

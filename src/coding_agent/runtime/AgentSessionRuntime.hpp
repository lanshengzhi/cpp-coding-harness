#pragma once

#include <cch/agent/Agent.hpp>
#include <cch/coding_agent/AgentSessionEvent.hpp>
#include <cch/coding_agent/AgentSessionSnapshot.hpp>
#include <cch/coding_agent/PromptTemplate.hpp>
#include "coding_agent/AgentSession.hpp"
#include <cch/coding_agent/Skill.hpp>
#include <cch/util/Error.hpp>
#include "coding_agent/prompt/SystemPromptBuilder.hpp"
#include "coding_agent/runtime/RuntimeServices.hpp"
#include "coding_agent/runtime/SessionEventCommitment.hpp"
#include "coding_agent/runtime/SessionLifecycle.hpp"
#include "coding_agent/runtime/SessionStats.hpp"
#include "coding_agent/runtime/UserBash.hpp"
#include "harness/compaction/Compaction.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/steady_timer.hpp>

#include <cstddef>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace cch::coding_agent::runtime {

struct AgentSessionRuntimeConfig {
    std::size_t max_queued_messages{agent::kDefaultMaxQueuedMessages};
    std::size_t max_queued_bytes{agent::kDefaultMaxQueuedBytes};
    /// Explicit turn cap forwarded to the Agent; std::nullopt imposes no cap
    /// (ADR 0015).
    std::optional<int> max_turns{std::nullopt};
    ai::Model model{};
    /// pi `scopedModels`: the `--models` scope carried into the session for
    /// Ctrl+P cycling. Session-only state; the interactive scoped-models
    /// selector replaces it at runtime.
    std::vector<coding_agent::ScopedModel> scoped_models{};
    /// pi `defaultThinkingLevel` from the merged settings scope. Used as the
    /// fresh-session and resumed-without-entry thinking level before the
    /// Agent's creation clamp (pi sdk.ts
    /// `settingsManager.getDefaultThinkingLevel() ?? DEFAULT_THINKING_LEVEL`);
    /// a resumed `thinking_level_change` entry wins over it (T04).
    std::optional<std::string> default_thinking_level{std::nullopt};
    /// pi `_rebuildSystemPrompt` inputs from the resource loader
    /// (`resourceLoader.getSystemPrompt()`): the custom system prompt text
    /// (`--system-prompt` text-or-file, else the discovered SYSTEM.md
    /// content) rendering as the custom-prompt branch.
    std::optional<std::string> custom_prompt{std::nullopt};
    /// pi `resourceLoader.getAppendSystemPrompt()`: the resolved append
    /// strings in source order; joined with `"\n\n"` into the append
    /// section (pi `_rebuildSystemPrompt`).
    std::vector<std::string> append_system_prompt;
    /// pi `resourceLoader.getAgentsFiles().agentsFiles`: the Project Context
    /// Files rendering as `<project_context>`/`<project_instructions
    /// path="...">`. Not Project Trust gated (pinned fact).
    std::vector<prompt::ProjectContextFile> context_files;
    /// pi `resourceLoader.getSystemPromptSource()`: the SYSTEM.md source
    /// path when the resolved custom prompt came from a file (loaded-resources
    /// Context presentation).
    std::optional<std::string> system_prompt_source{std::nullopt};
    /// pi `resourceLoader.getAppendSystemPromptSources()`: the
    /// APPEND_SYSTEM.md source paths when the append strings came from files.
    std::vector<std::string> append_system_prompt_sources;
    /// Per-kind loader diagnostics (pi `skillDiagnostics`/
    /// `promptDiagnostics`/`themeDiagnostics`) for the loaded-resources
    /// presentation and the `/reload` refresh.
    std::vector<ResourceDiagnostic> skill_diagnostics;
    std::vector<ResourceDiagnostic> prompt_diagnostics;
    std::vector<ResourceDiagnostic> theme_diagnostics;
    /// The resolved `ProjectResourceLoadingRequest` the session was assembled
    /// under (pi's retained `DefaultResourceLoader` options): `/reload`
    /// re-runs the same discovery with the creation-time trust state
    /// preserved.
    std::optional<ProjectResourceLoadingRequest> resource_loading_request{
        std::nullopt};
};

/// Resolved turn auto-retry settings (pi `settings-manager.ts`
/// `getRetrySettings`): `settings.retry` fields with pi's defaults applied
/// (`enabled: true`, `maxRetries: 3`, `baseDelayMs: 2000`, exponential
/// backoff `baseDelayMs * 2^(attempt-1)`).
struct RetrySettings {
    bool enabled{true};
    std::size_t max_retries{3};
    std::size_t base_delay_ms{2000};
};

/// Result of one `/reload` resource re-read (pi `resourceLoader.reload()`
/// results): the per-kind diagnostics and the re-discovered theme documents
/// the TUI re-registers through `discover_themes` (#418).
struct AgentSessionReloadResult {
    std::vector<ResourceDiagnostic> skill_diagnostics;
    std::vector<ResourceDiagnostic> prompt_diagnostics;
    std::vector<ResourceDiagnostic> theme_diagnostics;
    std::vector<LoadedThemeResource> themes;
};

/// Internal runtime behind AgentSession. Composes the stateful Agent with
/// session persistence, the pi-shaped System Prompt (built at session
/// construction), resources, and session presentation.
class AgentSessionRuntime {
public:
    AgentSessionRuntime(
        RuntimeServices services,
        OpenSession session,
        std::vector<Skill> skills,
        std::vector<PromptTemplate> templates,
        AgentSessionRuntimeConfig config);

    AgentSessionRuntime(const AgentSessionRuntime&) = delete;
    AgentSessionRuntime& operator=(const AgentSessionRuntime&) = delete;
    AgentSessionRuntime(AgentSessionRuntime&&) = delete;
    AgentSessionRuntime& operator=(AgentSessionRuntime&&) = delete;

    /// Reject the prompt at admission when the current model's provider has
    /// no configured auth (pi `agent-session.ts` `prompt()` preflight): a real
    /// model whose provider resolves no auth fails with pi's verbatim re-auth
    /// guidance (no-key branch, or the OAuth re-auth branch for OAuth-typed
    /// providers). The placeholder `kDefaultModel` is skipped: "no model" is
    /// not an auth failure, and streaming it fails through normal provider
    /// lookup ("Unknown provider: unknown") exactly like pi. `checkAuth`
    /// failures propagate unchanged.
    [[nodiscard]] boost::asio::awaitable<util::ExpectedVoid>
    preflight_auth_guidance();

    /// Run one prompt on the awaiting host executor through optional prompt
    /// interpretation, the stateful Agent, persistence, and event fanout.
    [[nodiscard]] boost::asio::awaitable<util::ExpectedVoid> run_prompt(
        std::string prompt,
        std::vector<ai::ImageContent> images,
        bool expand_prompt_templates,
        std::move_only_function<util::ExpectedVoid()> on_preflight_accepted = {});

    /// Private Native TUI path for one direct-user Shell execution. User Bash
    /// may overlap an active Agent run; a result completed mid-run stays
    /// pending and commits exactly once after the whole run settles.
    [[nodiscard]] bool has_user_shell() const { return services_.user_shell != nullptr; }
    /// Whether the session's project scope is trusted (pi
    /// `settingsManager.isProjectTrusted()`): false when assembly had no
    /// settings surface.
    [[nodiscard]] bool is_project_trusted() const {
        return services_.settings_manager &&
            services_.settings_manager->is_project_trusted();
    }
    [[nodiscard]] boost::asio::awaitable<util::Expected<UserBashCompletion>> run_user_bash(
        std::string command,
        bool exclude_from_context,
        UserBashProgressSink progress_sink);
    void cancel_user_bash();

    // ── Subscriptions ──────────────────────────────────────────────────────

    /// Subscribe through the authoritative stateful Agent weak-observer path.
    [[nodiscard]] util::Expected<agent::AgentEventSubscription> subscribe(
        agent::AgentEventSink sink);

    /// Subscribe a weak observer for session-assembly events (pi
    /// `AgentSessionEvent`): currently the turn auto-retry
    /// `auto_retry_start`/`auto_retry_end` events. Observer failures are
    /// diagnostic observations and deactivate the observer without vetoing
    /// retry progress (ADR 0017).
    [[nodiscard]] util::Expected<SessionEventSubscription> subscribe_session(
        AgentSessionEventSink sink);

    // ── Input queues ───────────────────────────────────────────────────────

    [[nodiscard]] util::ExpectedVoid steer(
        std::string text,
        std::vector<ai::ImageContent> images,
        bool expand_prompt_templates);
    [[nodiscard]] util::ExpectedVoid follow_up(
        std::string text,
        std::vector<ai::ImageContent> images,
        bool expand_prompt_templates);
    [[nodiscard]] util::ExpectedVoid set_steering_mode(agent::InputQueueMode mode);
    [[nodiscard]] util::ExpectedVoid set_follow_up_mode(agent::InputQueueMode mode);
    [[nodiscard]] util::ExpectedVoid clear_steering_queue();
    [[nodiscard]] util::ExpectedVoid clear_follow_up_queue();
    [[nodiscard]] util::ExpectedVoid clear_input_queues();

    // ── Model / thinking state ────────────────────────────────────────────

    /// Set the thinking level for subsequent turns (pi `AgentSession`
    /// `setThinkingLevel`). The level is clamped to the active model's
    /// supported set; on a real change the session appends a
    /// `thinking_level_change` entry and writes the global settings default
    /// (pi: `supportsThinking() || effectiveLevel !== "off"`), so resume
    /// restores the level exactly like pi (T04). Returns the effective
    /// (clamped) level, or an error for an invalid request or a persistence
    /// failure. Live Agent state advances first; a persistence failure is
    /// reported without rolling the change back (Session Event Commitment
    /// philosophy).
    [[nodiscard]] util::Expected<std::string> set_thinking_level(
        std::string_view level);

    /// Runtime model switch (pi `AgentSession.setModel`, G3 decision 5):
    /// validates that the target model's provider resolves auth (`No API key
    /// for <provider>/<model>` otherwise), swaps the live Agent model,
    /// appends the `model_change` session entry (skipped for in-memory
    /// sessions, like the creation-time entry), writes the global settings
    /// default provider/model, and re-clamps the thinking level against the
    /// new model's supported set (pi's `setModel` → `setThinkingLevel`
    /// sequence). Live Agent state advances first; a persistence failure is
    /// reported without rolling the change back (Session Event Commitment
    /// philosophy).
    [[nodiscard]] boost::asio::awaitable<util::ExpectedVoid> set_model(
        ai::Model model);

    /// Runtime model cycle (pi `AgentSession.cycleModel`, G3 decision 5):
    /// when the session carries scoped models, cycle within the auth-filtered
    /// scoped set (pi `_cycleScopedModel`: models whose provider resolves no
    /// auth are dropped; a scoped model's explicit thinking level overrides
    /// the current preference); otherwise cycle within the available models
    /// (pi `_cycleAvailableModel`). A set with zero or one eligible model
    /// yields `std::nullopt`. Each cycle applies the model, appends the
    /// `model_change` entry, writes the global settings default, and re-clamps
    /// the thinking level, exactly like `set_model`.
    [[nodiscard]] boost::asio::awaitable<util::Expected<std::optional<ModelCycleResult>>>
    cycle_model(std::string_view direction);

    /// Cycle the thinking level through the active model's supported set (pi
    /// `AgentSession.cycleThinkingLevel`): the next level after the current
    /// one, wrapping. `std::nullopt` when the active model supports no
    /// thinking. Applies `set_thinking_level` (entry + settings default on a
    /// real change).
    [[nodiscard]] util::Expected<std::optional<std::string>> cycle_thinking_level();

    /// Replace the session's scoped-model set (pi `setScopedModels`;
    /// session-only, never persisted). An empty set restores un-scoped
    /// cycling over the available models.
    void set_scoped_models(std::vector<coding_agent::ScopedModel> models);
    /// The session's scoped-model set (pi `scopedModels`).
    [[nodiscard]] const std::vector<coding_agent::ScopedModel>& scoped_models() const {
        return scoped_models_;
    }

    // ── Tree navigation (pi navigateTree, G2 decision 13) ──────────────────

    /// pi `waitForIdle`: settle when an Agent run is active. The run in
    /// flight continues to its normal terminal; a no-op when idle. User Bash
    /// and manual compaction are outside the Agent run and never awaited.
    [[nodiscard]] boost::asio::awaitable<util::ExpectedVoid> wait_for_idle();

    /// The session tree topology for the tree selector (pi
    /// `sessionManager.getTree()` + `getLeafId()`): root nodes with resolved
    /// labels and the current active leaf id (empty at the root position).
    /// Persisted sessions read the file's entries; in-memory sessions derive
    /// a linear tree from the live context with synthetic ids that only the
    /// tree surface understands (the in-memory store keeps no entries).
    [[nodiscard]] util::Expected<coding_agent::SessionTreeTopology>
    session_tree() const;

    /// pi `AgentSession.navigateTree` subset (G2 decision 13): switch the
    /// active path to `target_id` with the leaf/active-path semantics of the
    /// pi v3 Session Format — user/custom-message targets move the leaf to
    /// the parent (null at the root) and return the message text; other
    /// targets become the leaf. Persisted sessions persist a `leaf` marker
    /// and rebuild the live Agent context from the new path; in-memory
    /// sessions truncate the live context. Branch summarization generation
    /// stays Deferred: no `branch_summary` is ever produced. Rejects an
    /// active Agent run with pi's verbatim error.
    [[nodiscard]] util::Expected<coding_agent::TreeNavigationResult>
    navigate_tree(std::string_view target_id);

    /// pi `SessionManager.appendLabelChange` (the tree editLabel flow):
    /// append a `label` entry targeting `entry_id` under the current leaf.
    /// Persisted sessions write the entry; in-memory sessions keep no entry
    /// surface and the change is dropped like every in-memory store write.
    /// Verbatim pi errors: `Entry <id> not found` for an unknown target.
    [[nodiscard]] util::ExpectedVoid set_entry_label(
        std::string_view entry_id,
        std::optional<std::string> label);

    // ── Compaction ────────────────────────────────────────────────────────

    /// Manually compact the session context (pi `AgentSession.compact`):
    /// aborts the active run first, waits for it to settle, then runs the
    /// compaction machinery (`prepareCompaction`/`compact` in the harness
    /// module) through the session's `ModelRuntime`, persists the resulting
    /// `compaction` session entry, and rebuilds the live Agent context as
    /// compactionSummary + retained tail. Only persisted sessions (with a
    /// session file) have a tree/entry surface for compaction.
    [[nodiscard]] boost::asio::awaitable<util::Expected<coding_agent::CompactionResult>>
    compact(std::string custom_instructions);

    /// Outcome of the automatic compaction trigger policy (pi
    /// `AgentSession._checkCompaction`), consulted after each completed loop
    /// run and before each new prompt.
    enum class AutoCompactionOutcome {
        /// No compaction decision applied; the run is finished.
        None,
        /// Overflow compacted with retry; the session loop must continue.
        OverflowRetry,
        /// A second overflow after one compact-and-retry attempt; the prompt
        /// fails with pi's verbatim recovery message.
        OverflowRecoveryFailed,
        /// Compacted without retry (threshold, or overflow whose answer
        /// already completed); the run is finished.
        Compacted,
    };

    /// Automatic compaction trigger policy (pi `_checkCompaction`): overflow
    /// terminal errors route to compact-and-retry-once (a second overflow in
    /// the same prompt fails with pi's verbatim recovery message), threshold
    /// (`contextTokens > contextWindow − reserveTokens`) compacts with no
    /// retry, and overflow never routes to turn auto-retry. `skip_aborted_check`
    /// mirrors pi: the post-run check skips aborted messages while the
    /// pre-prompt check does not (so an aborted response still triggers the
    /// threshold path before the next prompt). Requires an idle Agent.
    [[nodiscard]] boost::asio::awaitable<AutoCompactionOutcome> check_auto_compaction(
        const ai::AssistantMessage& assistant_message,
        bool skip_aborted_check);

    // ── State accessors ────────────────────────────────────────────────────

    [[nodiscard]] AgentSessionSnapshot snapshot(
        const std::optional<std::filesystem::path>& session_path) const;
    [[nodiscard]] std::size_t message_count() const;
    [[nodiscard]] std::optional<std::string> last_assistant_text() const;

    /// pi `sessionManager.getSessionName()`: the trimmed name of the latest
    /// `session_info` entry for the current session. Persisted sessions read
    /// the file; in-memory sessions have no `session_info` surface and
    /// report none.
    [[nodiscard]] std::optional<std::string> session_name() const;

    /// pi `AgentSession.setSessionName`: sanitize the name (CR/LF runs become
    /// one space, then trimmed) and append a `session_info` entry under the
    /// current leaf. In-memory sessions keep no entry surface and the change
    /// is dropped like every in-memory store write. Returns the stored
    /// (sanitized) name.
    [[nodiscard]] util::Expected<std::optional<std::string>> set_session_name(
        std::string name);

    /// pi `getSessionStats` subset: per-role message counts and usage/token
    /// totals for the `/session` command.
    [[nodiscard]] SessionStats session_stats() const;
    [[nodiscard]] const std::string& session_id() const { return session_.metadata.session_id; }
    [[nodiscard]] std::optional<std::filesystem::path> session_path() const {
        return session_.store ? session_.store->path() : std::nullopt;
    }
    [[nodiscard]] const std::string& provider() const { return session_.metadata.provider; }
    [[nodiscard]] const std::string& model() const { return session_.metadata.model; }
    [[nodiscard]] std::shared_ptr<ModelRuntime> model_runtime() const {
        return services_.model_runtime;
    }
    [[nodiscard]] const std::filesystem::path& workspace() const { return session_.workspace; }
    [[nodiscard]] const std::vector<Skill>& skills() const;
    [[nodiscard]] const std::vector<PromptTemplate>& templates() const;
    /// pi `resourceLoader.getSystemPromptSource()`: the resolved SYSTEM.md
    /// source path (loaded-resources Context presentation).
    [[nodiscard]] const std::optional<std::string>& system_prompt_source() const {
        return config_.system_prompt_source;
    }
    /// pi `resourceLoader.getAppendSystemPromptSources()`: the resolved
    /// APPEND_SYSTEM.md source paths.
    [[nodiscard]] const std::vector<std::string>& append_system_prompt_sources() const {
        return config_.append_system_prompt_sources;
    }
    /// pi `resourceLoader.getAgentsFiles().agentsFiles`: the Project Context
    /// Files.
    [[nodiscard]] const std::vector<prompt::ProjectContextFile>& context_files() const {
        return config_.context_files;
    }
    /// pi `resourceLoader` per-kind diagnostics (`skillDiagnostics`/
    /// `promptDiagnostics`/`themeDiagnostics`).
    [[nodiscard]] const std::vector<ResourceDiagnostic>& skill_diagnostics() const {
        return config_.skill_diagnostics;
    }
    [[nodiscard]] const std::vector<ResourceDiagnostic>& prompt_diagnostics() const {
        return config_.prompt_diagnostics;
    }
    [[nodiscard]] const std::vector<ResourceDiagnostic>& theme_diagnostics() const {
        return config_.theme_diagnostics;
    }

    // ── Resource reload (pi `/reload`, #418) ───────────────────────────────

    /// pi `AgentSession.reload()` subset: re-read User Settings (preserving
    /// `projectTrusted`), re-run the retained `ProjectResourceLoadingRequest`
    /// against the session workspace with the creation-time trust state,
    /// swap skills/templates/prompt inputs, rebuild the System Prompt, and
    /// push it into the live Agent. Fatal loader errors abort with
    /// `std::unexpected` (the TUI shows `Reload failed: ...`). Requires an
    /// idle session (streaming/compaction refusal is the TUI's job via
    /// `is_streaming`/`is_compacting`).
    [[nodiscard]] boost::asio::awaitable<util::Expected<AgentSessionReloadResult>>
    reload();

    /// pi `isStreaming`: whether an Agent run is in flight (User Bash does
    /// NOT block `/reload`).
    [[nodiscard]] bool is_streaming() const { return prompt_active_; }
    /// pi `isCompacting`: whether a compaction is in flight.
    [[nodiscard]] bool is_compacting() const { return compaction_active_; }

    // ── Lifecycle ──────────────────────────────────────────────────────────

    /// Request cancellation of the active Agent prompt. Idempotent and a
    /// no-op while idle.
    void abort();

    [[nodiscard]] bool is_open() const {
        return lifecycle_ == Lifecycle::Open;
    }
    [[nodiscard]] bool is_busy() const {
        return lifecycle_ == Lifecycle::Closing || prompt_active_ ||
            user_bash_active_ || compaction_active_;
    }
    void close() noexcept;

    /// The current session's tree, or an empty optional when the session has
    /// no persisted entry surface (in-memory sessions keep no entries); a
    /// persisted file that cannot be opened is an error. Reused by
    /// `session_name`, `set_session_name`, and `session_stats`.
    [[nodiscard]] util::Expected<
        std::optional<harness::session::SessionTree>>
    open_session_tree() const;

private:
    /// Session lifecycle, tracked independently from the active-work facts so
    /// User Bash may overlap an Agent run (ADR 0026).
    enum class Lifecycle { Open, Closing, Closed };

    /// One completed User Bash execution whose commitment is deferred until
    /// the active Agent run settles. The signal is cancelled by the flush to
    /// release the awaiting run_user_bash coroutine, which then returns the
    /// completion or the commitment failure.
    struct PendingUserBashCommit {
        UserBashCompletion completion;
        boost::asio::steady_timer committed_signal;
        util::ExpectedVoid commit_result;
    };

    /// Shared preflight outcome for entry points that require a non-closed session.
    [[nodiscard]] util::ExpectedVoid reject_if_closed() const;
    /// Refresh the model Bash Tool's live PI_* session facts from the current
    /// Agent state (pi `resolveSpawnContext` reads `ctx.model`/
    /// `ctx.thinkingLevel` at execution time). Called after the Agent is
    /// constructed and after every model/thinking change.
    void refresh_bash_session_environment();
    /// pi `_getThinkingLevelForModelSwitch`: an explicit scoped-model level
    /// wins; otherwise a current model without thinking support falls back to
    /// the merged settings default (then pi's DEFAULT_THINKING_LEVEL);
    /// otherwise the current level is kept (re-clamped by the caller).
    [[nodiscard]] std::string resolve_thinking_level_for_switch(
        const std::optional<std::string>& explicit_level) const;
    /// Shared model-switch tail (pi `setModel`/`cycleModel` after the auth
    /// decision): swap the live Agent model, append the `model_change` entry,
    /// write the global settings default, and re-clamp the thinking level.
    [[nodiscard]] boost::asio::awaitable<util::ExpectedVoid> apply_model_switch(
        ai::Model model,
        std::string thinking_level);
    /// Shared preflight outcome for entry points that reject a concurrent prompt.
    [[nodiscard]] util::ExpectedVoid reject_if_busy() const;
    /// pi `_rebuildSystemPrompt`: build the System Prompt in pi's exact shape
    /// from the current `config_` prompt inputs, `skills_`, and tool metadata
    /// (the identity delta confined to the documentation paths). Called at
    /// construction and on `/reload`.
    [[nodiscard]] std::string rebuild_system_prompt() const;
    /// Shared preflight outcome rejecting a second concurrent User Bash.
    [[nodiscard]] util::ExpectedVoid reject_if_user_bash_busy() const;

    /// Commit one completed Bash message to Live Session State, then the
    /// Session Store. Store failure is reported on the completion diagnostic
    /// without rolling back Live Session State; a Live Session State failure
    /// is returned so the caller can reject the completion outright.
    [[nodiscard]] util::ExpectedVoid commit_user_bash_completion(
        UserBashCompletion& completion);
    /// Commit and release every deferred Bash completion in completion order.
    void flush_pending_user_bash();

    [[nodiscard]] boost::asio::awaitable<util::ExpectedVoid> run_agent_loop(
        ai::UserMessage prompt,
        std::stop_source stop_source);
    /// The compaction body with `compaction_active_` already set; the public
    /// `compact` wrapper resets the flag on every exit path.
    [[nodiscard]] boost::asio::awaitable<util::Expected<coding_agent::CompactionResult>>
    compact_impl(std::string custom_instructions);
    /// Auto-compaction body (pi `_runAutoCompaction`): prepare, summarize
    /// through the session's `ModelRuntime`, persist, and rebuild context.
    /// Returns whether the run should continue (`will_retry`). Skips silently
    /// when the session is unpersisted, has no model, has nothing to compact,
    /// or summarization fails (pi returns false in every such case). `reason`
    /// feeds the emitted `compaction_start`/`compaction_end` events.
    [[nodiscard]] boost::asio::awaitable<bool> run_auto_compaction(
        bool will_retry,
        std::string reason);
    /// Shared compaction execution for the manual trigger and the auto policy:
    /// summarize `preparation` through the session's `ModelRuntime`, persist
    /// the `compaction` entry, and rebuild the live Agent context as
    /// compactionSummary + retained tail.
    [[nodiscard]] boost::asio::awaitable<util::Expected<coding_agent::CompactionResult>>
    execute_compaction(
        const harness::session::CompactionPreparation& preparation,
        const harness::session::CompactionSettings& settings,
        std::string custom_instructions);
    /// Resolve the effective compaction settings from the merged settings
    /// scope with pi's `DEFAULT_COMPACTION_SETTINGS` applied to missing fields.
    [[nodiscard]] harness::session::CompactionSettings effective_compaction_settings() const;
    /// Resolve the effective retry settings from the merged settings
    /// scope with pi's defaults applied to missing fields.
    [[nodiscard]] RetrySettings effective_retry_settings() const;
    /// Whether the failed assistant message is retryable by the turn
    /// auto-retry policy (pi `_isRetryableError`): context overflow is never
    /// retryable (compaction owns it, T10), otherwise pi's
    /// `isRetryableAssistantError` classification applies.
    [[nodiscard]] bool is_retryable_error(
        const ai::AssistantMessage& message) const;
    /// pi `_prepareRetry`: increment the attempt budget, emit
    /// `auto_retry_start`, remove the failed assistant message from live
    /// state (it stays in session history), and wait an abort-interruptible
    /// exponential-backoff sleep. Returns whether the caller should continue
    /// the agent; an aborted sleep emits `auto_retry_end` with pi's
    /// "Retry cancelled" and returns false (exactly one terminal outcome).
    [[nodiscard]] boost::asio::awaitable<bool> prepare_retry(
        const ai::AssistantMessage& message,
        std::stop_token stop_token);
    /// Deliver one session-assembly event to every registered observer.
    void emit_session_event(const AgentSessionEvent& event);
    /// Timestamp of the latest `compaction` entry on the active branch, or
    /// nullopt when none exists (pi `getLatestCompactionEntry`).
    [[nodiscard]] std::optional<ai::TimestampMs> latest_compaction_timestamp() const;
    [[nodiscard]] boost::asio::awaitable<void> finalize_close_after_active_work();
    [[nodiscard]] std::shared_ptr<harness::AsyncExecutionEnv> release_close_resources() noexcept;
    void finalize_close() noexcept;

    RuntimeServices services_;
    OpenSession session_;
    /// Immutable skill/template snapshots loaded at session creation (pi
    /// `_resourceLoader` results the session was assembled under). The
    /// System Prompt is built from the skills at construction; `/skill:`
    /// expansion and prompt-template expansion read the same snapshots.
    std::vector<Skill> skills_;
    std::vector<PromptTemplate> templates_;
    /// Tool prompt metadata collected before the move-only tool registry
    /// moved into the Agent (pi `_toolPromptSnippets`/
    /// `_toolPromptGuidelines`): `/reload` rebuilds the System Prompt from
    /// these retained snippets/guidelines.
    std::vector<std::string> prompt_selected_tools_;
    std::map<std::string, std::string> prompt_tool_snippets_;
    std::vector<std::string> prompt_tool_guidelines_;
    // Declared after the borrowed client/store owners so it is destroyed first.
    std::optional<agent::Agent> agent_;
    /// Request-time re-auth guidance decorator (pi `_getRequiredRequestAuth`):
    /// the Agent's stream and the summarization seam run through a
    /// session-layer runtime that rewrites auth/oauth-category terminal
    /// failures to pi's two verbatim guidance branches. Holds the canonical
    /// runtime alive through the session.
    std::shared_ptr<ModelRuntime> auth_guided_runtime_;

    AgentSessionRuntimeConfig config_;
    Lifecycle lifecycle_{Lifecycle::Open};
    bool prompt_active_{false};
    bool user_bash_active_{false};
    bool compaction_active_{false};
    /// pi `_overflowRecoveryAttempted`: true from the first overflow
    /// compact-and-retry until a new user message starts (or a non-error
    /// assistant message completes in pi); a second overflow while true fails
    /// with pi's verbatim recovery message.
    bool overflow_recovery_attempted_{false};
    /// pi `_scopedModels`: the session's scoped-model set for Ctrl+P cycling,
    /// seeded from the `--models` CLI scope and replaced session-only by the
    /// scoped-models selector. Empty = cycle over the available models.
    std::vector<coding_agent::ScopedModel> scoped_models_{};
    /// pi `_retryAttempt`: the in-flight turn auto-retry attempt count, reset
    /// by a non-error assistant message completion, by the final-failure
    /// emission, or by an aborted backoff.
    int retry_attempt_{0};
    /// Weak session-event observer registry (pi `AgentSessionEvent`
    /// listeners). Failing observers are deactivated; the shared anchor keeps
    /// subscription handles safe after the runtime is destroyed.
    struct SessionSubscriptionAnchor {
        AgentSessionRuntime* runtime{nullptr};
    };
    /// The session-event subscription handle unregisters its observer from
    /// the registry below.
    friend class coding_agent::SessionEventSubscription;
    std::shared_ptr<SessionSubscriptionAnchor> session_event_anchor_{
        std::make_shared<SessionSubscriptionAnchor>(this)};
    struct SessionSubscriber {
        std::size_t id{0};
        AgentSessionEventSink sink;
        bool registered{true};
        bool delivery_enabled{true};
    };
    std::vector<std::shared_ptr<SessionSubscriber>> session_event_observers_;
    /// Next session-event subscriber id.
    std::size_t next_session_subscriber_id_{1};
    /// Bounded, redacted diagnostics for session-event observer failures (the
    /// session-assembly mirror of the Agent's weak-observer diagnostics,
    /// ADR 0017). Exposed through `AgentSessionSnapshot::session_event_diagnostics`.
    std::vector<util::Error> session_event_diagnostics_;
    std::vector<std::shared_ptr<PendingUserBashCommit>> pending_user_bash_;
    std::optional<std::stop_source> active_stop_source_;
    std::optional<std::stop_source> active_user_bash_stop_source_;
    /// Released (cancelled) when the active prompt settles; a concurrent
    /// manual compaction awaits it after requesting run cancellation.
    std::optional<boost::asio::steady_timer> prompt_settled_signal_;
};

} // namespace cch::coding_agent::runtime

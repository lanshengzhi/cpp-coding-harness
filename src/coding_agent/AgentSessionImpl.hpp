#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// Private implementation machinery for AgentSession: the complete
// AgentSession::Impl definition shared by the AgentSession translation units
// (AgentSession.cpp, AgentSessionExecution.cpp, AgentSessionCompaction.cpp,
// AgentSessionInteraction.cpp). This is not a second callable interface: only
// the session implementation units include it; hosts and tests drive the
// AgentSession interface.
// ─────────────────────────────────────────────────────────────────────────────

#include "coding_agent/AgentSession.hpp"

#include <cch/agent/Agent.hpp>
#include <cch/coding_agent/ModelRuntime.hpp>
#include "coding_agent/runtime/AgentSessionAssembly.hpp"
#include "coding_agent/runtime/SessionPersistence.hpp"
#include "coding_agent/runtime/UserBash.hpp"
#include "agent/harness/compaction/Compaction.hpp"

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

namespace cch::coding_agent {

/// Resolved turn auto-retry settings (pi `settings-manager.ts`
/// `getRetrySettings`): `settings.retry` fields with pi's defaults applied
/// (`enabled: true`, `maxRetries: 3`, `baseDelayMs: 2000`, exponential
/// backoff `baseDelayMs * 2^(attempt-1)`).
struct RetrySettings {
    bool enabled{true};
    std::size_t max_retries{3};
    std::size_t base_delay_ms{2000};
};

/// The Agent Session implementation: the stateful Agent composed with
/// session persistence, the pi-shaped System Prompt (built at session
/// construction), resources, and session presentation. Owned through the
/// AgentSession handle's shared_ptr so a lazy coroutine admitted before the
/// public handle moves or is destroyed keeps the implementation alive.
struct AgentSession::Impl {
    explicit Impl(runtime::AgentSessionAssembly assembly);
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    /// Reject the prompt at admission when the current model's provider has
    /// no configured auth (pi `agent-session.ts` `prompt()` preflight): a real
    /// model whose provider resolves no auth fails with pi's verbatim re-auth
    /// guidance (no-key branch, or the OAuth re-auth branch for OAuth-typed
    /// providers). The placeholder `kDefaultModel` is skipped: "no model" is
    /// not an auth failure, and streaming it fails through normal provider
    /// lookup ("Unknown provider: unknown") exactly like pi. `checkAuth`
    /// failures propagate unchanged.
    [[nodiscard]] boost::asio::awaitable<support::ExpectedVoid> preflight_auth_guidance();

    /// Run one prompt on the awaiting host executor through optional prompt
    /// interpretation, the stateful Agent, persistence, and event fanout.
    [[nodiscard]] boost::asio::awaitable<support::ExpectedVoid> run_prompt(
            std::string prompt, std::vector<ai::ImageContent> images, bool expand_prompt_templates);

    /// Private Native TUI path for one direct-user Shell execution. User Bash
    /// may overlap an active Agent run; a result completed mid-run stays
    /// pending and commits exactly once after the whole run settles.
    [[nodiscard]] bool has_user_shell() const { return services_.user_shell != nullptr; }
    /// Whether the session's project scope is trusted (pi
    /// `settingsManager.isProjectTrusted()`): false when assembly had no
    /// settings surface.
    [[nodiscard]] bool is_project_trusted() const {
        return services_.settings_manager && services_.settings_manager->is_project_trusted();
    }
    [[nodiscard]] boost::asio::awaitable<support::Expected<runtime::UserBashCompletion>> run_user_bash(
            std::string command, bool exclude_from_context, runtime::UserBashProgressSink progress_sink);
    void cancel_user_bash();

    // ── Input queues ───────────────────────────────────────────────────────

    [[nodiscard]] support::ExpectedVoid steer(
            std::string text, std::vector<ai::ImageContent> images, bool expand_prompt_templates);
    [[nodiscard]] support::ExpectedVoid follow_up(
            std::string text, std::vector<ai::ImageContent> images, bool expand_prompt_templates);
    [[nodiscard]] support::ExpectedVoid set_steering_mode(agent::InputQueueMode mode);
    [[nodiscard]] support::ExpectedVoid set_follow_up_mode(agent::InputQueueMode mode);
    [[nodiscard]] support::ExpectedVoid clear_steering_queue();
    [[nodiscard]] support::ExpectedVoid clear_follow_up_queue();
    [[nodiscard]] support::ExpectedVoid clear_input_queues();

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
    [[nodiscard]] support::Expected<std::string> set_thinking_level(std::string_view level);

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
    [[nodiscard]] boost::asio::awaitable<support::ExpectedVoid> set_model(ai::Model model);

    /// Runtime model cycle (pi `AgentSession.cycleModel`, G3 decision 5):
    /// when the session carries scoped models, cycle within the auth-filtered
    /// scoped set (pi `_cycleScopedModel`: models whose provider resolves no
    /// auth are dropped; a scoped model's explicit thinking level overrides
    /// the current preference); otherwise cycle within the available models
    /// (pi `_cycleAvailableModel`). A set with zero or one eligible model
    /// yields `std::nullopt`. Each cycle applies the model, appends the
    /// `model_change` entry, writes the global settings default, and re-clamps
    /// the thinking level, exactly like `set_model`.
    [[nodiscard]] boost::asio::awaitable<support::Expected<std::optional<ModelCycleResult>>> cycle_model(
            std::string_view direction);

    /// Cycle the thinking level through the active model's supported set (pi
    /// `AgentSession.cycleThinkingLevel`): the next level after the current
    /// one, wrapping. `std::nullopt` when the active model supports no
    /// thinking. Applies `set_thinking_level` (entry + settings default on a
    /// real change).
    [[nodiscard]] support::Expected<std::optional<std::string>> cycle_thinking_level();

    /// Replace the session's scoped-model set (pi `setScopedModels`;
    /// session-only, never persisted). An empty set restores un-scoped
    /// cycling over the available models.
    void set_scoped_models(std::vector<ScopedModel> models);
    /// The session's scoped-model set (pi `scopedModels`).
    [[nodiscard]] const std::vector<ScopedModel>& scoped_models() const { return scoped_models_; }

    // ── Tree navigation (pi navigateTree, G2 decision 13) ──────────────────

    /// pi `waitForIdle`: settle when an Agent run is active. The run in
    /// flight continues to its normal terminal; a no-op when idle. User Bash
    /// and manual compaction are outside the Agent run and never awaited.
    [[nodiscard]] boost::asio::awaitable<support::ExpectedVoid> wait_for_idle();

    /// The session tree topology for the tree selector (pi
    /// `sessionManager.getTree()` + `getLeafId()`): root nodes with resolved
    /// labels and the current active leaf id (empty at the root position).
    /// Both persistence alternatives answer from the store's live tree —
    /// the in-memory store keeps the same entries pi's non-persisting
    /// SessionManager keeps, so no tree surface synthesizes topology from
    /// the live context.
    [[nodiscard]] support::Expected<SessionTreeTopology> session_tree() const;

    /// pi `AgentSession.navigateTree` subset (G2 decision 13): switch the
    /// active path to `target_id` with the leaf/active-path semantics of the
    /// pi v3 Session Format — user/custom-message targets move the leaf to
    /// the parent (null at the root) and return the message text; other
    /// targets become the leaf. The leaf marker append moves the live tree's
    /// leaf (persisted sessions also durably record it), and the live Agent
    /// context rebuilds from the new path — identical for both persistence
    /// alternatives. Branch summarization generation stays Deferred: no
    /// `branch_summary` is ever produced. Rejects an active Agent run with
    /// pi's verbatim error.
    [[nodiscard]] support::Expected<TreeNavigationResult> navigate_tree(std::string_view target_id);

    /// pi `SessionManager.appendLabelChange` (the tree editLabel flow):
    /// append a `label` entry targeting `entry_id` under the current leaf.
    /// Both persistence alternatives mirror the entry into the store's live
    /// tree; persisted sessions also write it durably. Verbatim pi errors:
    /// `Entry <id> not found` for an unknown target.
    [[nodiscard]] support::ExpectedVoid set_entry_label(std::string_view entry_id, std::optional<std::string> label);

    // ── Compaction ────────────────────────────────────────────────────────

    /// Manually compact the session context (pi `AgentSession.compact`):
    /// aborts the active run first, waits for it to settle, then runs the
    /// compaction machinery (`prepareCompaction`/`compact` in the harness
    /// module) through the session's `ModelRuntime`, persists the resulting
    /// `compaction` session entry, and rebuilds the live Agent context as
    /// compactionSummary + retained tail. Only persisted sessions (with a
    /// session file) have a tree/entry surface for compaction.
    [[nodiscard]] boost::asio::awaitable<support::Expected<CompactionResult>> compact(std::string custom_instructions);

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
            const ai::AssistantMessage& assistant_message, bool skip_aborted_check);

    // ── State accessors ────────────────────────────────────────────────────

    [[nodiscard]] AgentSessionSnapshot snapshot() const;
    [[nodiscard]] std::size_t message_count() const;
    [[nodiscard]] std::optional<std::string> last_assistant_text() const;

    /// pi `sessionManager.getSessionName()`: the trimmed name of the latest
    /// `session_info` entry for the current session. Persisted sessions read
    /// the file; in-memory sessions have no `session_info` surface and
    /// report none.
    [[nodiscard]] std::optional<std::string> session_name() const;

    /// pi `AgentSession.setSessionName`: sanitize the name (CR/LF runs become
    /// one space, then trimmed) and append a `session_info` entry under the
    /// current leaf. The `session_info` surface stays scoped to persisted
    /// sessions; the in-memory change is dropped. Returns the stored
    /// (sanitized) name.
    [[nodiscard]] support::Expected<std::optional<std::string>> set_session_name(std::string name);

    /// pi `getSessionStats` subset: per-role message counts and usage/token
    /// totals for the `/session` command.
    [[nodiscard]] runtime::SessionStats session_stats() const;
    [[nodiscard]] const std::string& session_id() const { return session_.metadata.session_id; }
    [[nodiscard]] const std::string& provider() const { return session_.metadata.provider; }
    [[nodiscard]] const std::string& model() const { return session_.metadata.model; }
    [[nodiscard]] std::shared_ptr<ModelRuntime> model_runtime() const { return services_.model_runtime; }
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
    [[nodiscard]] const std::vector<prompt::ProjectContextFile>& context_files() const { return config_.context_files; }
    /// pi `resourceLoader` per-kind diagnostics (`skillDiagnostics`/
    /// `promptDiagnostics`/`themeDiagnostics`).
    [[nodiscard]] const std::vector<ResourceDiagnostic>& skill_diagnostics() const { return config_.skill_diagnostics; }
    [[nodiscard]] const std::vector<ResourceDiagnostic>& prompt_diagnostics() const {
        return config_.prompt_diagnostics;
    }
    [[nodiscard]] const std::vector<ResourceDiagnostic>& theme_diagnostics() const { return config_.theme_diagnostics; }

    // ── Resource reload (pi `/reload`, #418) ───────────────────────────────

    /// pi `AgentSession.reload()` subset: re-read User Settings (preserving
    /// `projectTrusted`), re-run the retained `ProjectResourceLoadingRequest`
    /// against the session workspace with the creation-time trust state,
    /// swap skills/templates/prompt inputs, rebuild the System Prompt, and
    /// push it into the live Agent. Fatal loader errors abort with
    /// `std::unexpected` (the TUI shows `Reload failed: ...`). Requires an
    /// idle session; the Native TUI performs the user-facing refusal before
    /// admission and this door repeats the guard for races.
    [[nodiscard]] boost::asio::awaitable<support::Expected<AgentSessionReloadResult>> reload(
            std::stop_token stop_token);

    /// pi `isStreaming`: whether an Agent run is in flight (User Bash does
    /// NOT block `/reload`).
    [[nodiscard]] bool is_streaming() const { return prompt_active_; }
    /// pi `isCompacting`: whether a compaction is in flight.
    [[nodiscard]] bool is_compacting() const { return compaction_active_; }

    // ── Lifecycle ──────────────────────────────────────────────────────────

    /// Request cancellation of the active Agent prompt. Idempotent and a
    /// no-op while idle.
    void abort();

    [[nodiscard]] bool is_open() const { return lifecycle_ == Lifecycle::Open; }
    [[nodiscard]] bool is_busy() const {
        return lifecycle_ == Lifecycle::Closing || prompt_active_ || user_bash_active_ || compaction_active_;
    }
    /// Idempotent Close request (ADR 0011/0040, issue #467): stops new work
    /// admission first, then requests cancellation of the active prompt and
    /// User Bash, and waits for every admitted prompt, manual compaction,
    /// Session Event Commitment, and admitted callback to reach its terminal
    /// outcome before releasing Session resources exactly once. An admitted
    /// compaction is awaited, never cancelled; a Close requested from inside
    /// an admitted callback returns without waiting on that callback.
    void close() noexcept;

    /// Session lifecycle, tracked independently from the active-work facts so
    /// User Bash may overlap an Agent run (ADR 0026).
    enum class Lifecycle { Open, Closing, Closed };

    /// One completed User Bash execution whose commitment is deferred until
    /// the active Agent run settles. The signal is cancelled by the flush to
    /// release the awaiting run_user_bash coroutine, which then returns the
    /// completion or the commitment failure.
    struct PendingUserBashCommit {
        runtime::UserBashCompletion completion;
        boost::asio::steady_timer committed_signal;
        support::ExpectedVoid commit_result;
    };

    /// Shared preflight outcome for entry points that require a non-closed session.
    [[nodiscard]] support::ExpectedVoid reject_if_closed() const;
    /// Refresh the model Bash Tool's live PI_* session facts from the current
    /// Agent state (pi `resolveSpawnContext` reads `ctx.model`/
    /// `ctx.thinkingLevel` at execution time). Called after the Agent is
    /// constructed and after every model/thinking change.
    void refresh_bash_session_environment();
    /// pi `_getThinkingLevelForModelSwitch`: an explicit scoped-model level
    /// wins; otherwise a current model without thinking support falls back to
    /// the merged settings default (then pi's DEFAULT_THINKING_LEVEL);
    /// otherwise the current level is kept (re-clamped by the caller).
    [[nodiscard]] std::string resolve_thinking_level_for_switch(const std::optional<std::string>& explicit_level) const;
    /// Shared model-switch tail (pi `setModel`/`cycleModel` after the auth
    /// decision): swap the live Agent model, append the `model_change` entry,
    /// write the global settings default, and re-clamp the thinking level.
    [[nodiscard]] boost::asio::awaitable<support::ExpectedVoid> apply_model_switch(
            ai::Model model, std::string thinking_level);
    /// Shared preflight outcome for entry points that reject a concurrent prompt.
    [[nodiscard]] support::ExpectedVoid reject_if_busy() const;
    /// pi `_rebuildSystemPrompt`: build the System Prompt in pi's exact shape
    /// from the current `config_` prompt inputs, `skills_`, and tool metadata
    /// (the identity delta confined to the documentation paths). Called at
    /// construction and on `/reload`.
    [[nodiscard]] std::string rebuild_system_prompt() const;
    /// Shared preflight outcome rejecting a second concurrent User Bash.
    [[nodiscard]] support::ExpectedVoid reject_if_user_bash_busy() const;

    /// Commit one completed Bash message to Live Session State, then the
    /// Session Store. Store failure is reported on the completion diagnostic
    /// without rolling back Live Session State; a Live Session State failure
    /// is returned so the caller can reject the completion outright.
    [[nodiscard]] support::ExpectedVoid commit_user_bash_completion(runtime::UserBashCompletion& completion);
    /// Commit and release every deferred Bash completion in completion order.
    void flush_pending_user_bash();

    [[nodiscard]] boost::asio::awaitable<support::ExpectedVoid> run_agent_loop(
            ai::UserMessage prompt, std::stop_source stop_source);
    /// The compaction body with `compaction_active_` already set; the public
    /// `compact` wrapper resets the flag on every exit path.
    [[nodiscard]] boost::asio::awaitable<support::Expected<CompactionResult>> compact_impl(
            std::string custom_instructions);
    /// Auto-compaction body (pi `_runAutoCompaction`): prepare, summarize
    /// through the session's `ModelRuntime`, persist, and rebuild context.
    /// Returns whether the run should continue (`will_retry`). Skips silently
    /// when the session is unpersisted, has no model, has nothing to compact,
    /// or summarization fails (pi returns false in every such case). `reason`
    /// feeds the emitted `compaction_start`/`compaction_end` events.
    [[nodiscard]] boost::asio::awaitable<bool> run_auto_compaction(bool will_retry, std::string reason);
    /// Shared compaction execution for the manual trigger and the auto
    /// policy: run the harness compaction door over the session store. The
    /// branch assembly, preparation, skip reasons, and summarization stay
    /// inside the module; `on_compaction_start` fires inside the door after a
    /// successful preparation (the auto trigger's `compaction_start` point;
    /// the manual trigger emits before entering the door and passes nothing).
    [[nodiscard]] boost::asio::awaitable<support::Expected<harness::session::CompactionOutcomeVariant>>
    attempt_compaction(std::string custom_instructions, harness::session::CompactionStartHook on_compaction_start);
    /// Persist a successful harness result as the `compaction` session entry
    /// and rebuild the live Agent context as compactionSummary + retained
    /// tail (the append/rebuild half of pi `AgentSession.compact`).
    [[nodiscard]] boost::asio::awaitable<support::Expected<CompactionResult>> commit_compaction(
            harness::session::CompactionResult result);
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
    [[nodiscard]] bool is_retryable_error(const ai::AssistantMessage& message) const;
    /// pi `_prepareRetry`: increment the attempt budget, emit
    /// `auto_retry_start`, remove the failed assistant message from live
    /// state (it stays in session history), and wait an abort-interruptible
    /// exponential-backoff sleep. Returns whether the caller should continue
    /// the agent; an aborted sleep emits `auto_retry_end` with pi's
    /// "Retry cancelled" and returns false (exactly one terminal outcome).
    [[nodiscard]] boost::asio::awaitable<bool> prepare_retry(
            const ai::AssistantMessage& message, std::stop_token stop_token);
    /// Deliver one session-assembly event to every registered observer.
    void emit_session_event(const AgentSessionEvent& event);
    /// Timestamp of the latest `compaction` entry on the active branch, or
    /// nullopt when none exists (pi `getLatestCompactionEntry`).
    [[nodiscard]] std::optional<ai::TimestampMs> latest_compaction_timestamp() const;
    [[nodiscard]] boost::asio::awaitable<void> finalize_close_after_active_work();
    [[nodiscard]] std::shared_ptr<harness::AsyncExecutionEnv> release_close_resources() noexcept;
    void finalize_close() noexcept;
    /// Build the session's AI-owned `ModelStream` factory: the runtime's
    /// Models catalog composed with pi's request-time re-auth guidance
    /// (ADR 0040 / #453). Each call returns an independent, re-invocable
    /// factory; the Agent and the summarization seam each hold one.
    [[nodiscard]] ai::ModelStreamFactory make_stream_factory();

    runtime::RuntimeServices services_;
    runtime::OpenSession session_;
    /// The session's off-loop Session Event Commitment channel (ADR 0040):
    /// admitted message appends persist on Runtime workers and their outcomes
    /// return through the session's serialized mailbox in FIFO order. Null
    /// for in-memory sessions and sessions without a persistent store.
    std::shared_ptr<runtime::SessionPersistence> persistence_;
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

    runtime::AgentSessionConfig config_;
    /// The resolved session path recorded at assembly (bind time); unlike
    /// `session_.store`, it is retained through Session Close so the
    /// handle's session_path()/snapshot() introspection keeps answering.
    std::optional<std::filesystem::path> session_path_;
    Lifecycle lifecycle_{Lifecycle::Open};
    bool prompt_active_{false};
    bool user_bash_active_{false};
    bool compaction_active_{false};
    /// Resource reload is admitted independently from prompt/compaction work;
    /// Close requests its cancellation and waits for the reload frame to
    /// release its retained filesystem capabilities before final teardown.
    bool reload_active_{false};
    std::stop_source reload_stop_source_;
    /// pi `_overflowRecoveryAttempted`: true from the first overflow
    /// compact-and-retry until a new user message starts (or a non-error
    /// assistant message completes in pi); a second overflow while true fails
    /// with pi's verbatim recovery message.
    bool overflow_recovery_attempted_{false};
    /// pi `_scopedModels`: the session's scoped-model set for Ctrl+P cycling,
    /// seeded from the `--models` CLI scope and replaced session-only by the
    /// scoped-models selector. Empty = cycle over the available models.
    std::vector<ScopedModel> scoped_models_{};
    /// pi `_retryAttempt`: the in-flight turn auto-retry attempt count, reset
    /// by a non-error assistant message completion, by the final-failure
    /// emission, or by an aborted backoff.
    int retry_attempt_{0};
    /// Weak session-event observer registry (pi `AgentSessionEvent`
    /// listeners). Failing observers are deactivated; the shared anchor keeps
    /// subscription handles safe after the session is destroyed.
    struct SessionSubscriptionAnchor {
        Impl* impl{nullptr};
    };
    std::shared_ptr<SessionSubscriptionAnchor> session_event_anchor_{std::make_shared<SessionSubscriptionAnchor>(this)};
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
    std::vector<support::Error> session_event_diagnostics_;
    std::vector<std::shared_ptr<PendingUserBashCommit>> pending_user_bash_;
    std::optional<std::stop_source> active_stop_source_;
    std::optional<std::stop_source> active_user_bash_stop_source_;
    /// Released (cancelled) when the active prompt settles; a concurrent
    /// manual compaction awaits it after requesting run cancellation.
    std::optional<boost::asio::steady_timer> prompt_settled_signal_;
};

namespace detail {

// ── Lazy-coroutine session entries ──────────────────────────────────────────
// Each entry is the coroutine half of an AgentSession async operation. The
// public handle's ordinary (non-coroutine) member passes its impl_ copy as
// the by-value first argument, so the shared_ptr enters the coroutine frame
// synchronously at the call: moving or destroying the public handle before
// the first co_await cannot invalidate the returned lazy awaitable.

/// Prompt entry (defined in AgentSessionExecution.cpp).
[[nodiscard]] boost::asio::awaitable<support::ExpectedVoid> session_prompt(std::shared_ptr<AgentSession::Impl> impl,
        std::string text,
        std::vector<ai::ImageContent> images,
        bool expand_prompt_templates);

/// pi `waitForIdle` entry (defined in AgentSessionExecution.cpp).
[[nodiscard]] boost::asio::awaitable<support::ExpectedVoid> session_wait_for_idle(
        std::shared_ptr<AgentSession::Impl> impl);

/// Manual compaction entry (defined in AgentSessionCompaction.cpp).
[[nodiscard]] boost::asio::awaitable<support::Expected<CompactionResult>> session_compact(
        std::shared_ptr<AgentSession::Impl> impl, std::string custom_instructions);

/// Runtime model switch entry (defined in AgentSessionInteraction.cpp).
[[nodiscard]] boost::asio::awaitable<support::ExpectedVoid> session_set_model(
        std::shared_ptr<AgentSession::Impl> impl, ai::Model model);

/// Runtime model cycle entry (defined in AgentSessionInteraction.cpp).
[[nodiscard]] boost::asio::awaitable<support::Expected<std::optional<ModelCycleResult>>> session_cycle_model(
        std::shared_ptr<AgentSession::Impl> impl, std::string direction);

/// Resource reload entry (defined in AgentSessionInteraction.cpp).
[[nodiscard]] boost::asio::awaitable<support::Expected<AgentSessionReloadResult>> session_reload(
        std::shared_ptr<AgentSession::Impl> impl, std::stop_token stop_token);

/// One admission shaping for every user input path (Prompt, steering,
/// follow-up): optional skill/prompt-template expansion, then image content
/// appended to one complete user Agent Message. Defined in
/// AgentSessionExecution.cpp; shared with the input-queue admissions.
[[nodiscard]] ai::UserMessage make_admitted_user_message(std::string text,
        const std::vector<Skill>& skills,
        const std::vector<PromptTemplate>& templates,
        std::vector<ai::ImageContent> images,
        bool expand_prompt_templates);

} // namespace detail

} // namespace cch::coding_agent

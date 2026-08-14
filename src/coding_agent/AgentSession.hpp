#pragma once

#include <cch/agent/AgentContext.hpp>
#include <cch/agent/AgentEvent.hpp>
#include <cch/agent/AgentTool.hpp>
#include <cch/ai/Content.hpp>
#include <cch/ai/Model.hpp>
#include <cch/ai/Models.hpp>
#include <cch/ai/Usage.hpp>
#include <cch/coding_agent/AgentSessionEvent.hpp>
#include "coding_agent/ProjectResourceLoader.hpp"
#include <cch/coding_agent/AgentSessionSnapshot.hpp>
#include <cch/coding_agent/ModelResolver.hpp>
#include <cch/coding_agent/ModelRuntime.hpp>
#include <cch/coding_agent/PromptTemplate.hpp>
#include <cch/coding_agent/Skill.hpp>
#include <cch/agent/harness/session/SessionEntry.hpp>
#include <cch/util/Error.hpp>
#include <cch/util/JsonValue.hpp>
#include "coding_agent/runtime/AgentSessionCreationRequest.hpp"
#include "coding_agent/runtime/SessionFork.hpp"
#include "coding_agent/runtime/SessionStats.hpp"

#include <boost/asio/awaitable.hpp>

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// Internal session handle for the CLI frontends (pi `agent-session.ts`
// equivalent). The former public embeddable SDK surface (`Sdk.hpp`) is
// removed with the pi-coding-agent phase (ADR 0036): no public session header,
// no host-supplied skill/template/tool injection, no create_agent_session
// options surface. This header is private to the binary and its tests.
// ─────────────────────────────────────────────────────────────────────────────

namespace cch::coding_agent {
namespace detail {
class AgentSessionInteractiveAccess;
class AgentSessionPromptAccess;
class AgentSessionRuntimeAccess;
}
namespace runtime {
class AsyncUserShell;
struct AgentSessionReloadResult;
}

// ── Diagnostics ──────────────────────────────────────────────────────────────

/// Diagnostic produced during session creation.
struct SessionDiagnostic {
    enum class Severity { Info, Warning, Error };

    Severity severity{Severity::Warning};
    /// Stable machine-readable code (e.g. "resource:project_skills",
    /// "duplicate:duplicate_skill_skipped").
    std::string code;
    /// Human-readable message.
    std::string message;
    /// Associated filesystem path, if any.
    std::optional<std::string> path;
};

// ── CreateAgentSessionResult ─────────────────────────────────────────────────

class AgentSession;

/// Result of a successful session creation.
struct CreateAgentSessionResult {
    /// The created/resumed session handle. Move-only.
    std::unique_ptr<AgentSession> session;
    /// Diagnostics collected during creation (provider fallback,
    /// resource load warnings, etc.).
    std::vector<SessionDiagnostic> diagnostics;
    /// pi `modelFallbackMessage` (sdk.ts `createAgentSession`): set when a
    /// persisted session's stored `model_change` identity could not be
    /// restored against the live runtime. Surfaces as an interactive boot
    /// warning ("Warning: <message>") and is dropped in print mode; never a
    /// stderr diagnostic.
    std::optional<std::string> model_fallback_message;

    /// Theme documents collected by the resource loader (pi
    /// `resourceLoader.getThemes()`): the interactive boot registers and
    /// applies them through the theme controller (`setRegisteredThemes` +
    /// `applyFromSettings`). Parsing stays in the TUI layer.
    std::vector<LoadedThemeResource> theme_resources;

    /// Resolved session metadata (for host introspection).
    std::string session_id;
    std::string provider;
    std::string model;
    /// Actual persisted session path. This remains optional so the contract
    /// can represent session targets without a physical file.
    std::optional<std::filesystem::path> session_path;
    std::filesystem::path workspace;
    /// Full session metadata captured at creation/resume time.
    harness::session::SessionMetadata metadata;
};

// ── PromptOptions ────────────────────────────────────────────────────────────

/// Per-prompt options passed to AgentSession::prompt().
struct PromptOptions {
    /// When false, bypass skill and prompt-template expansion and send the raw
    /// text to the agent loop.
    bool expand_prompt_templates{true};
    /// Image content appended after the prompt text in the resulting user
    /// message. Values pass through unchanged to the selected provider.
    std::vector<ai::ImageContent> images;
};

// ── CompactionResult ─────────────────────────────────────────────────────────

/// Result of a manual session compaction (pi `CompactionResult`).
struct CompactionResult {
    /// Summary text replacing the compacted history.
    std::string summary;
    /// Entry id where retained history starts (pi `firstKeptEntryId`).
    std::string first_kept_entry_id;
    /// Estimated context tokens before compaction (pi `tokensBefore`).
    std::size_t tokens_before{0};
    /// Estimated context tokens after compaction (pi `estimatedTokensAfter`).
    std::optional<std::size_t> estimated_tokens_after{std::nullopt};
    /// Usage from the summarization call(s), when reported.
    std::optional<ai::Usage> usage{std::nullopt};
    /// pi `CompactionDetails`: `{readFiles, modifiedFiles}`.
    std::optional<util::JsonValue> details{std::nullopt};
};

// ── EventSubscription ────────────────────────────────────────────────────────

/// RAII handle for a subscriber callback.
/// Destroying or calling unsubscribe() stops event delivery.
/// No-op after session close.
class EventSubscription {
public:
    EventSubscription() = default;
    EventSubscription(EventSubscription&&) noexcept;
    EventSubscription& operator=(EventSubscription&&) noexcept;
    ~EventSubscription();
    EventSubscription(const EventSubscription&) = delete;
    EventSubscription& operator=(const EventSubscription&) = delete;

    /// Unsubscribe from further events. Idempotent.
    void unsubscribe();

    /// True while the callback remains registered. A close request retains
    /// active-run subscriber storage until callbacks quiesce, then invalidates
    /// every remaining handle.
    [[nodiscard]] explicit operator bool() const;

public:
    /// Opaque implementation type. Defined in the session implementation.
    struct Impl;

private:
    friend class AgentSession;
    std::unique_ptr<Impl> impl_;
};

// ── ModelCycleResult ─────────────────────────────────────────────────────────

/// pi `ModelCycleResult` (agent-session.ts): the model a cycle landed on,
/// the effective (clamped) thinking level, and whether the cycle ran within
/// the scoped set.
struct ModelCycleResult {
    ai::Model model{};
    std::string thinking_level{};
    bool is_scoped{false};
};

// ── Tree navigation (pi navigateTree, G2 decision 13) ────────────────────────

/// Result of one in-session tree navigation (pi `navigateTree` result
/// subset): `editorText` for user/custom-message targets (the message text
/// pre-fills the editor), absent for other targets; `cancelled` is always
/// false in the C++ subset (the summarize/extension cancel paths are
/// Deferred with branch summarization generation).
struct TreeNavigationResult {
    std::optional<std::string> editor_text{std::nullopt};
    bool cancelled{false};
};

/// The session tree topology for the tree selector (pi `getTree()` +
/// `getLeafId()`): root nodes with resolved labels and the current active
/// leaf id (empty when the leaf sits before the first entry).
struct SessionTreeTopology {
    std::vector<harness::session::SessionTreeNode> roots;
    std::string leaf_id;
};

// ── AgentSession ─────────────────────────────────────────────────────────────

/// Move-only session handle. Created by create_agent_session().
///
/// Lifecycle: Open → (prompt)* → Closed.
///   - prompt() is awaitable and serial; reentrant calls return an error.
///   - abort() idempotently requests cancellation of the active prompt and is
///     a no-op while idle; the session remains reusable after quiescence.
///   - close() is a synchronous, non-blocking, idempotent request. It rejects
///     new admission and cancels active work immediately, then releases
///     subscribers and owned resources after active callbacks quiesce.
///
/// Async operations and state access are confined to the host executor driving
/// prompt(); unrelated concurrent-thread access is not supported. AgentSession
/// owns no prompt executor or background thread.
///
/// State accessors (message_count(), last_assistant_text(), ...) reflect live
/// history as each message completes. A completed message remains visible if
/// later subscriber delivery or persistence fails.
class AgentSession {
public:
    /// Opaque implementation type. Defined in the session implementation
    /// source.
    struct Impl;

    AgentSession();
    AgentSession(AgentSession&&) noexcept;
    AgentSession& operator=(AgentSession&&) noexcept;
    ~AgentSession();
    AgentSession(const AgentSession&) = delete;
    AgentSession& operator=(const AgentSession&) = delete;

    // ── Prompt execution ─────────────────────────────────────────────────

    /// Run a text prompt with optional image content to completion on the
    /// awaiting coroutine's Asio executor.
    /// Progress is delivered through persistent subscriptions, and resulting
    /// state is queried separately. Provider rejection before runtime transport
    /// and infrastructure failures such as a provider event sink or persistence
    /// return explicit errors. An accepted provider error or aborted outcome
    /// completes normally; its final Assistant Message is delivered through the
    /// ordinary lifecycle and retained in state. Closed, busy, or other
    /// agent-execution failures also return errors.
    [[nodiscard]] boost::asio::awaitable<util::ExpectedVoid> prompt(
        std::string text,
        PromptOptions options = {});

    /// Blocking convenience facade over prompt(). It creates and drains a
    /// temporary executor for this call. Do not invoke it from callbacks or an
    /// execution context already driving a blocking prompt for this session;
    /// such self-wait attempts are rejected. Async hosts should use prompt().
    ///
    /// The temporary executor drives the shared ModelRuntime, which is not
    /// internally synchronized: do not call prompt_blocking on one thread while
    /// another thread drives the same session or any session sharing the same
    /// runtime.
    [[nodiscard]] util::ExpectedVoid prompt_blocking(
        std::string text,
        PromptOptions options = {});

    // ── Input queues ─────────────────────────────────────────────────────

    /// Queue input after the current assistant turn. Admission is synchronous
    /// and rejects before mutation when the Agent-owned queue is full.
    [[nodiscard]] util::ExpectedVoid steer(
        std::string text,
        PromptOptions options = {});

    /// Queue input only when the Agent would otherwise stop. Admission is
    /// synchronous and rejects before mutation when the Agent-owned queue is full.
    [[nodiscard]] util::ExpectedVoid follow_up(
        std::string text,
        PromptOptions options = {});

    /// Change the pi-compatible drain policy for steering input.
    [[nodiscard]] util::ExpectedVoid set_steering_mode(agent::InputQueueMode mode);

    /// Change the pi-compatible drain policy for follow-up input.
    [[nodiscard]] util::ExpectedVoid set_follow_up_mode(agent::InputQueueMode mode);

    /// Remove all pending steering input.
    [[nodiscard]] util::ExpectedVoid clear_steering_queue();

    /// Remove all pending follow-up input.
    [[nodiscard]] util::ExpectedVoid clear_follow_up_queue();

    /// Remove all pending steering and follow-up input.
    [[nodiscard]] util::ExpectedVoid clear_input_queues();

    // ── Model / thinking state ────────────────────────────────────────────

    /// Set the thinking level for subsequent turns (pi `AgentSession`
    /// `setThinkingLevel`). The level is validated and clamped to the active
    /// model's supported set; on a real change the session persists a
    /// `thinking_level_change` entry and the global settings default, so
    /// resume restores the level exactly like pi (T04). Returns the effective
    /// (clamped) level, or an error for an invalid request or a persistence
    /// failure.
    [[nodiscard]] util::Expected<std::string> set_thinking_level(
        std::string_view level);

    /// Runtime model switch (pi `AgentSession.setModel`): validates that the
    /// target model's provider resolves auth (`No API key for
    /// <provider>/<model>` otherwise), swaps the live Agent model, persists
    /// the `model_change` session entry and the global settings default, and
    /// re-clamps the thinking level against the new model. Same impl_ copying
    /// contract as prompt(): the returned lazy awaitable survives moving or
    /// destroying the public handle before its first co_await.
    [[nodiscard]] boost::asio::awaitable<util::ExpectedVoid> set_model(
        ai::Model model);
    /// Blocking facade for tests and one-shot hosts; drives the async path on
    /// a temporary executor (same contract as prompt_blocking).
    [[nodiscard]] util::ExpectedVoid set_model_blocking(ai::Model model);

    /// Runtime model cycle (pi `AgentSession.cycleModel`): when the session
    /// carries scoped models, cycle within the auth-filtered scoped set
    /// (models whose provider resolves no auth are dropped; a scoped model's
    /// explicit thinking level overrides the current preference); otherwise
    /// cycle within the available models. A set with zero or one eligible
    /// model yields `std::nullopt`. Same impl_ copying contract as prompt().
    [[nodiscard]] boost::asio::awaitable<util::Expected<std::optional<ModelCycleResult>>>
    cycle_model(std::string direction);
    /// Blocking facade for tests and one-shot hosts; drives the async path on
    /// a temporary executor (same contract as prompt_blocking).
    [[nodiscard]] util::Expected<std::optional<ModelCycleResult>> cycle_model_blocking(
        std::string direction);

    /// Cycle the thinking level through the active model's supported set (pi
    /// `AgentSession.cycleThinkingLevel`): the next level after the current
    /// one, wrapping. `std::nullopt` when the active model supports no
    /// thinking. Applies `set_thinking_level` (entry + settings default on a
    /// real change).
    [[nodiscard]] util::Expected<std::optional<std::string>> cycle_thinking_level();

    /// Replace the session's scoped-model set (pi `AgentSession.setScopedModels`;
    /// session-only, never persisted). An empty set restores un-scoped cycling
    /// over the available models.
    void set_scoped_models(std::vector<ScopedModel> models);
    /// The session's scoped-model set (pi `AgentSession.scopedModels`),
    /// seeded from the `--models` CLI scope.
    [[nodiscard]] const std::vector<ScopedModel>& scoped_models() const;

    // ── Session fork (pi `AgentSessionRuntime.fork` preparation) ──────────

    /// pi `getUserMessagesForForking`: every user message with non-blank text
    /// in session order. Persisted sessions read the file's entries (real
    /// entry ids); in-memory sessions derive messages from the live history
    /// with synthetic ids that only the fork flow understands.
    [[nodiscard]] std::vector<runtime::UserForkMessage>
    get_user_messages_for_forking() const;

    /// pi `AgentSessionRuntime.fork` preparation: validates the entry and
    /// computes the branched target (`ForkPosition::Before` requires a user
    /// message and branches before it; `ForkPosition::At` branches at the
    /// entry itself). Persisted sessions write the branched session file
    /// into the source session's directory; in-memory sessions produce the
    /// branch seed for a newly created in-memory session. Verbatim pi
    /// errors: "Invalid entry ID for forking", "This session has not been
    /// saved yet. Wait for the first assistant response before cloning or
    /// forking it.", "Failed to create forked session", and the G3 "Cannot
    /// fork: ..." source-file strings.
    [[nodiscard]] util::Expected<runtime::ForkPreparation> prepare_fork(
        std::string_view entry_id,
        runtime::ForkPosition position) const;

    // ── Tree navigation (pi navigateTree, G2 decision 13) ──────────────────

    /// pi `waitForIdle`: an awaitable that settles when an active Agent run
    /// finishes (the run already in flight continues to its normal terminal).
    /// Returns immediately when no run is active; User Bash and manual
    /// compaction are outside the Agent run and never awaited. The settle
    /// wait itself cannot fail, so the expected alternative stays empty.
    [[nodiscard]] boost::asio::awaitable<util::ExpectedVoid> wait_for_idle();

    /// The session tree topology for the tree selector (pi
    /// `sessionManager.getTree()` + `getLeafId()`): root nodes with resolved
    /// labels and the current active leaf. Persisted sessions read the
    /// file's entries; in-memory sessions derive a linear tree from the live
    /// context with synthetic ids that only the tree surface understands
    /// (the C++ in-memory store keeps no entries, #409).
    [[nodiscard]] util::Expected<SessionTreeTopology> session_tree() const;

    /// pi `AgentSession.navigateTree` subset: switch the active path to
    /// `target_id` with the leaf/active-path semantics of the pi v3 Session
    /// Format — a user or custom-message target moves the leaf to its parent
    /// (null at the root) and returns the message text for the editor; any
    /// other target becomes the leaf. Persisted sessions persist a `leaf`
    /// marker and rebuild the live Agent context from the new path; the
    /// in-memory path truncates the live context to the target. Branch
    /// summarization generation stays Deferred (G2): no `branch_summary` is
    /// ever produced, and `branch_summary` entries from pi-created sessions
    /// render unchanged. Rejects an active Agent run with pi's verbatim
    /// error; label attachment is the separate `set_entry_label` surface
    /// (editLabel).
    [[nodiscard]] util::Expected<TreeNavigationResult> navigate_tree(
        std::string_view target_id);

    /// pi `SessionManager.appendLabelChange` (the tree editLabel flow):
    /// append a `label` entry targeting `entry_id` under the current leaf.
    /// Persisted sessions write the entry; in-memory sessions keep no entry
    /// surface and the change is dropped like every in-memory store write.
    [[nodiscard]] util::ExpectedVoid set_entry_label(
        std::string_view entry_id,
        std::optional<std::string> label);

    // ── Compaction ───────────────────────────────────────────────────────

    /// Manually compact the session context (pi `AgentSession.compact`). The
    /// active run is aborted first, then the pre-cut history is summarized
    /// through `ModelRuntime::streamSimple` with `cacheRetention: "none"` and
    /// a fresh session id, a `compaction` session entry is persisted, and the
    /// live context is rebuilt as compactionSummary + retained tail. Returns
    /// the compaction result, or an error when the session is closed, a
    /// compaction is already in flight, no model is selected, the session has
    /// nothing to compact (or was already compacted), or summarization fails
    /// or is aborted.
    [[nodiscard]] boost::asio::awaitable<util::Expected<CompactionResult>>
    compact(std::string custom_instructions = {});

    // ── Event subscriptions ──────────────────────────────────────────────

    /// Subscribe to agent lifecycle events. The sink is called for every
    /// event emitted during subsequent prompts. Returns a handle; events
    /// stop when the handle is destroyed or unsubscribed.
    /// Returns an error if the session is closed.
    [[nodiscard]] util::Expected<EventSubscription> subscribe(
        agent::AgentEventSink sink);

    /// Subscribe to session-assembly events (pi `AgentSessionEvent`): the
    /// turn auto-retry `auto_retry_start`/`auto_retry_end` events. The sink
    /// is called for every session event during subsequent prompts. Returns a
    /// handle; events stop when the handle is destroyed or unsubscribed.
    /// Returns an error if the session is closed.
    [[nodiscard]] util::Expected<SessionEventSubscription> subscribe_session(
        AgentSessionEventSink sink);

    // ── State accessors ──────────────────────────────────────────────────

    /// Copy one independent snapshot of authoritative Agent state plus Session
    /// metadata and active-path topology. Like prompt() and other state access,
    /// snapshot() is confined to the executor driving this session; it performs
    /// no dispatch, callback, persistence, or Agent reentry and is safe to call
    /// from a lifecycle subscriber on that executor, including during a run.
    [[nodiscard]] AgentSessionSnapshot snapshot() const;

    /// Number of messages in live history.
    [[nodiscard]] std::size_t message_count() const;

    /// Last assistant text from live history, absent if no assistant message
    /// has completed.
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
    /// totals for the `/session` command (persisted sessions aggregate over
    /// the file's entries so compacted-away history still counts, like pi).
    [[nodiscard]] runtime::SessionStats session_stats() const;

    /// Session identifier.
    [[nodiscard]] const std::string& session_id() const;

    /// Actual persisted session path, absent when a target has no physical file.
    [[nodiscard]] const std::optional<std::filesystem::path>& session_path() const;

    /// Resolved provider name.
    [[nodiscard]] const std::string& provider() const;

    /// Resolved model name.
    [[nodiscard]] const std::string& model() const;

    /// The session's canonical model/auth runtime (ADR 0029). Held as a
    /// `shared_ptr`; runtimes are reusable across sessions and expose live
    /// `refresh()`/`login()`/`logout()` to all holders.
    [[nodiscard]] std::shared_ptr<ModelRuntime> model_runtime() const;

    /// Workspace path.
    [[nodiscard]] const std::filesystem::path& workspace() const;

    // ── Lifecycle ────────────────────────────────────────────────────────

    /// Request cancellation of the active prompt. Idempotent and a no-op when
    /// no prompt is active. Cancellation completes through the ordinary
    /// assistant `aborted` message, turn, and agent lifecycle; prompt() does
    /// not gain a second terminal-result channel. Calls are executor-confined
    /// under the same contract as prompt() and state access.
    void abort();

    /// Request session close. Synchronous, non-blocking, idempotent, and safe
    /// from an event callback. New work is rejected immediately and an active
    /// prompt receives the same cancellation request as abort(); subscriber
    /// and owned-resource teardown occurs exactly once after active callbacks
    /// and operations quiesce. Host-provided execution environments retain
    /// their ownership contract and are never cleaned up by the session.
    void close() noexcept;

    /// True while the session is open (not closed).
    [[nodiscard]] bool is_open() const;

    /// True while a prompt is in flight.
    [[nodiscard]] bool is_busy() const;

    // ── Resource reload (pi `/reload`, #418) ────────────────────────────

    /// pi `AgentSession.reload()`: re-read User Settings (preserving
    /// `projectTrusted`), skills, prompt templates, Project Context Files,
    /// and the SYSTEM/APPEND sources through the retained loader request,
    /// rebuild the System Prompt, and push it into the live Agent. The
    /// returned result carries the per-kind diagnostics and theme documents
    /// the TUI re-registers. The caller (the TUI) refuses while
    /// `is_streaming()`/`is_compacting()`; User Bash does not block reload.
    /// Same `impl_` copying contract as `prompt()`.
    [[nodiscard]] boost::asio::awaitable<
        util::Expected<runtime::AgentSessionReloadResult>>
    reload();

    /// pi `isStreaming`: whether an Agent run is in flight (User Bash does
    /// NOT block `/reload`).
    [[nodiscard]] bool is_streaming() const;
    /// pi `isCompacting`: whether a compaction is in flight.
    [[nodiscard]] bool is_compacting() const;

    // ── Resource access (read-only introspection) ────────────────────────

    /// Skills available to the agent (loaded project skills).
    [[nodiscard]] const std::vector<Skill>& skills() const;

    /// Prompt templates available for expansion.
    [[nodiscard]] const std::vector<PromptTemplate>& templates() const;

    /// pi `resourceLoader.getSystemPromptSource()`: the resolved SYSTEM.md
    /// source path (loaded-resources Context presentation).
    [[nodiscard]] const std::optional<std::string>& system_prompt_source() const;
    /// pi `resourceLoader.getAppendSystemPromptSources()`: the resolved
    /// APPEND_SYSTEM.md source paths.
    [[nodiscard]] const std::vector<std::string>& append_system_prompt_sources() const;
    /// pi `resourceLoader.getAgentsFiles().agentsFiles`: the Project Context
    /// Files (loaded-resources Context presentation).
    [[nodiscard]] const std::vector<prompt::ProjectContextFile>& context_files() const;
    /// pi `resourceLoader` per-kind diagnostics (`skillDiagnostics`/
    /// `promptDiagnostics`/`themeDiagnostics`).
    [[nodiscard]] const std::vector<ResourceDiagnostic>& skill_diagnostics() const;
    [[nodiscard]] const std::vector<ResourceDiagnostic>& prompt_diagnostics() const;
    [[nodiscard]] const std::vector<ResourceDiagnostic>& theme_diagnostics() const;

private:
    friend util::Expected<CreateAgentSessionResult> create_agent_session(
        runtime::AgentSessionCreationRequest request);
    friend util::Expected<CreateAgentSessionResult> create_agent_session_for_testing(
        runtime::AgentSessionCreationRequest request,
        std::shared_ptr<ai::Models> models);
    friend util::Expected<CreateAgentSessionResult> create_agent_session_for_testing(
        runtime::AgentSessionCreationRequest request,
        std::shared_ptr<ai::Models> models,
        std::unique_ptr<runtime::AsyncUserShell> user_shell);
    friend class detail::AgentSessionInteractiveAccess;
    friend class detail::AgentSessionPromptAccess;
    friend class detail::AgentSessionRuntimeAccess;

    // Shared only with an active prompt frame so reentrant close/destruction
    // cannot invalidate runtime capabilities before callbacks quiesce.
    std::shared_ptr<Impl> impl_;
};

// ── Factory ──────────────────────────────────────────────────────────────────

/// Create or resume an agent session from the CLI creation request.
///
/// Validates the request, resolves the workspace, opens or creates the
/// selected persisted session, assembles the provider client, execution
/// environment, tools, and resources, and returns a session handle with
/// diagnostics.
///
/// Does not write to stdout/stderr or read RPC stdin.
[[nodiscard]] util::Expected<CreateAgentSessionResult> create_agent_session(
    runtime::AgentSessionCreationRequest request);

/// Private test-support wrapper around SessionFactory's Models assembly seam
/// (the deterministic provider surface the deleted fake-provider CLI flag
/// used to drive).
[[nodiscard]] util::Expected<CreateAgentSessionResult> create_agent_session_for_testing(
    runtime::AgentSessionCreationRequest request,
    std::shared_ptr<ai::Models> models);

/// Private test-support wrapper around SessionFactory's Models assembly seam
/// with an injected Session-owned User Shell.
[[nodiscard]] util::Expected<CreateAgentSessionResult> create_agent_session_for_testing(
    runtime::AgentSessionCreationRequest request,
    std::shared_ptr<ai::Models> models,
    std::unique_ptr<runtime::AsyncUserShell> user_shell);

} // namespace cch::coding_agent

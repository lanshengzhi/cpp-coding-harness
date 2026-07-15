#pragma once

#include <cch/agent/AgentEvent.hpp>
#include <cch/agent/AgentTool.hpp>
#include <cch/ai/ChatClient.hpp>
#include <cch/coding_agent/PromptTemplate.hpp>
#include <cch/coding_agent/ProjectResources.hpp>
#include <cch/coding_agent/ProjectTrust.hpp>
#include <cch/coding_agent/Skill.hpp>
#include <cch/harness/ExecutionEnv.hpp>
#include <cch/harness/session/SessionEntry.hpp>
#include <cch/util/Error.hpp>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cch::coding_agent {
namespace detail {
class AgentSessionPromptAccess;
class AgentSessionRuntimeAccess;
}

// ─────────────────────────────────────────────────────────────────────────────
// Public SDK types for the embeddable C++23 agent surface.
//
// This header is a source-level API contract. It does not expose private
// runtime headers, serialization machinery, CLI/RPC helpers, or provider
// wire DTOs. The SDK is experimental and not ABI-stable.
// ─────────────────────────────────────────────────────────────────────────────

// ── Diagnostics ──────────────────────────────────────────────────────────────

/// Diagnostic produced during session creation.
struct SdkDiagnostic {
    enum class Severity { Info, Warning, Error };

    Severity severity{Severity::Warning};
    /// Stable machine-readable code (e.g. "provider_config_fallback",
    /// "resource:project_skills", "duplicate:duplicate_skill_skipped").
    std::string code;
    /// Human-readable message.
    std::string message;
    /// Associated filesystem path, if any.
    std::optional<std::string> path;
};

// ── Provider configuration ───────────────────────────────────────────────────

/// Configuration for constructing the default OpenAI-compatible chat client.
/// Ignored when a host-provided chat client is supplied.
struct SdkProviderConfig {
    std::string provider;
    std::string model;
    std::optional<std::string> base_url;
    /// Environment variable chain to resolve the API key from.
    /// The first set and non-empty variable wins.
    std::optional<std::vector<std::string>> api_key_env;
};

// ── Built-in tool selection ──────────────────────────────────────────────────

/// Which built-in tools to register.
/// Defaults match the safe-tool posture: read, write, edit_file; bash opt-in.
struct SdkBuiltinTools {
    bool read{true};
    bool write{true};
    bool edit_file{true};
    bool bash{false};
};

// ── CreateAgentSessionOptions ────────────────────────────────────────────────

/// Options passed to create_agent_session().
///
/// Session target: exactly one of `session_path` (create new) or
/// `resume_path` (resume existing) must be set. Both-set and neither-set
/// are validation errors.
///
/// Workspace: required for new sessions. For resumes, if `workspace` is
/// explicit and differs from the stored session workspace, creation fails.
///
/// Provider: if no `chat_client` is supplied, `provider_config` is used to
/// construct a default OpenAI-compatible client via the provider registry.
/// If neither is supplied, creation fails.
struct CreateAgentSessionOptions {
    // ── Session target ───────────────────────────────────────────────────
    std::optional<std::filesystem::path> session_path;
    std::optional<std::filesystem::path> resume_path;
    std::filesystem::path workspace;

    // ── Provider configuration ───────────────────────────────────────────
    /// Configuration for default provider client construction.
    /// Ignored when `chat_client` is set.
    std::optional<SdkProviderConfig> provider_config;

    // ── Host-provided capabilities ───────────────────────────────────────
    /// Host-provided streaming chat client. If set, provider_config is
    /// ignored for execution and a diagnostic notes that metadata is
    /// host-provided. Ownership transfers to the session.
    std::unique_ptr<ai::StreamingChatClient> chat_client;
    /// Host-provided execution environment. If not set, a local execution
    /// environment is constructed for the workspace. Host-provided environments
    /// are never cleaned up by the session.
    std::shared_ptr<harness::AsyncExecutionEnv> execution_env;

    // ── Built-in tool selection ──────────────────────────────────────────
    SdkBuiltinTools builtin_tools{};

    // ── Custom tools ─────────────────────────────────────────────────────
    /// Custom tools whose ownership transfers to the session. Duplicate names
    /// (including clashes with built-in tools) fail session creation.
    std::vector<std::unique_ptr<agent::AsyncAgentTool>> custom_tools;

    // ── Host-provided resources ──────────────────────────────────────────
    /// Skills available to the agent. Host-provided skills take precedence
    /// over project-discovered duplicates.
    std::vector<Skill> skills;
    /// Prompt templates available for expansion before the agent loop.
    /// Host-provided templates take precedence over project-discovered
    /// duplicates.
    std::vector<PromptTemplate> prompt_templates;

    // ── Project resource loading (opt-in) ────────────────────────────────
    /// When true, discover and load project-local skills and prompt
    /// templates from `.cpp-harness/`. Diagnostics are returned as values,
    /// not printed.
    bool load_project_resources{false};
    /// Default trust decision for project resource loading. Defaults to `ask`.
    std::optional<DefaultProjectTrust> default_project_trust;
    /// Resource enablement for project skills.
    std::optional<ResourceEnablement> project_skills_enablement;
    /// Optional absolute path to a user-controlled trust store outside the
    /// workspace. If omitted, the user-level default `~/.cpp-harness/trust.json`
    /// is used. A supplied path must be absolute and must not resolve to the
    /// workspace or any path inside it.
    std::optional<std::filesystem::path> trust_store_path;

    // ── Reserved ─────────────────────────────────────────────────────────
    /// Max agent turns per prompt (passed through to AsyncAgentOptions).
    int max_turns{30};
};

// ── CreateAgentSessionResult ─────────────────────────────────────────────────

class AgentSession;

/// Result of a successful create_agent_session() call.
struct CreateAgentSessionResult {
    /// The created/resumed session handle. Move-only.
    std::unique_ptr<AgentSession> session;
    /// Diagnostics collected during creation (provider fallback,
    /// resource load warnings, etc.).
    std::vector<SdkDiagnostic> diagnostics;

    /// Resolved session metadata (for host introspection).
    std::string session_id;
    std::string provider;
    std::string model;
    std::filesystem::path session_path;
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

    /// True while subscribed and session open.
    [[nodiscard]] explicit operator bool() const;

public:
    /// Opaque implementation type. Defined in the SDK implementation.
    struct Impl;

private:
    friend class AgentSession;
    std::unique_ptr<Impl> impl_;
};

// ── AgentSession ─────────────────────────────────────────────────────────────

/// Move-only session handle. Created by create_agent_session().
///
/// Lifecycle: Open → (prompt)* → Closed.
///   - prompt() is blocking and serial; reentrant calls return an error.
///   - close() is idempotent; prompts after close return an error.
///   - subscribers are cleared on close.
///
/// State accessors (message_count(), last_assistant_text(), ...) reflect live
/// history as each message completes. A completed message remains visible if
/// later subscriber delivery or persistence fails.
class AgentSession {
public:
    /// Opaque implementation type. Defined in the SDK implementation source.
    struct Impl;

    AgentSession();
    AgentSession(AgentSession&&) noexcept;
    AgentSession& operator=(AgentSession&&) noexcept;
    ~AgentSession();
    AgentSession(const AgentSession&) = delete;
    AgentSession& operator=(const AgentSession&) = delete;

    // ── Prompt execution ─────────────────────────────────────────────────

    /// Run a blocking prompt to completion. Progress is delivered through
    /// persistent subscriptions, and resulting state is queried separately.
    /// Returns an error if the session is closed, busy, event delivery or
    /// persistence fails, or agent execution otherwise fails.
    [[nodiscard]] util::ExpectedVoid prompt(
        std::string text,
        PromptOptions options = {});

    // ── Event subscriptions ──────────────────────────────────────────────

    /// Subscribe to agent lifecycle events. The sink is called for every
    /// event emitted during subsequent prompts. Returns a handle; events
    /// stop when the handle is destroyed or unsubscribed.
    /// Returns an error if the session is closed.
    [[nodiscard]] util::Expected<EventSubscription> subscribe(
        agent::AgentEventSink sink);

    // ── State accessors ──────────────────────────────────────────────────

    /// Number of messages in live history.
    [[nodiscard]] std::size_t message_count() const;

    /// Last assistant text from live history, absent if no assistant message
    /// has completed.
    [[nodiscard]] std::optional<std::string> last_assistant_text() const;

    /// Session identifier.
    [[nodiscard]] const std::string& session_id() const;

    /// Path to the session JSONL file.
    [[nodiscard]] const std::filesystem::path& session_path() const;

    /// Resolved provider name.
    [[nodiscard]] const std::string& provider() const;

    /// Resolved model name.
    [[nodiscard]] const std::string& model() const;

    /// Workspace path.
    [[nodiscard]] const std::filesystem::path& workspace() const;

    // ── Lifecycle ────────────────────────────────────────────────────────

    /// Close the session. Idempotent. Clears all subscribers. Releases
    /// tools, resources, and SDK-owned execution environment.
    /// Host-provided execution environments are not cleaned up unless the
    /// host explicitly requests it (deferred to future options).
    [[nodiscard]] util::ExpectedVoid close();

    /// True while the session is open (not closed).
    [[nodiscard]] bool is_open() const;

    /// True while a prompt is in flight.
    [[nodiscard]] bool is_busy() const;

    // ── Resource access (read-only introspection) ────────────────────────

    /// Skills available to the agent (host-provided + loaded project skills).
    [[nodiscard]] const std::vector<Skill>& skills() const;

    /// Prompt templates available for expansion.
    [[nodiscard]] const std::vector<PromptTemplate>& templates() const;

private:
    friend util::Expected<CreateAgentSessionResult> create_agent_session(
        CreateAgentSessionOptions options);
    friend class detail::AgentSessionPromptAccess;
    friend class detail::AgentSessionRuntimeAccess;

    std::unique_ptr<Impl> impl_;
};

// ── Factory ──────────────────────────────────────────────────────────────────

/// Create or resume an agent session.
///
/// Validates options, opens/creates the JSONL session, assembles the provider
/// client, execution environment, tools, and resources, and returns a session
/// handle with diagnostics.
///
/// Does not write to stdout/stderr or read RPC stdin.
[[nodiscard]] util::Expected<CreateAgentSessionResult> create_agent_session(
    CreateAgentSessionOptions options);

} // namespace cch::coding_agent

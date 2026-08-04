#pragma once

#include <cch/ai/Models.hpp>
#include <cch/coding_agent/Sdk.hpp>
#include <cch/coding_agent/Settings.hpp>
#include "coding_agent/runtime/AgentSessionRuntime.hpp"
#include "coding_agent/runtime/AsyncUserShell.hpp"
#include "coding_agent/runtime/SessionLifecycle.hpp"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <optional>
#include <vector>

namespace cch::coding_agent::runtime {

/// Internal creation request shared by CLI/RPC adapters. Public SDK creation
/// options are adapted separately so the public SDK interface does not absorb
/// CLI-specific facts.
struct AgentSessionCreationRequest {
    bool fake{false};
    bool enable_bash{false};
    /// Assemble the Native TUI's independent Session-owned User Shell
    /// capability (ADR 0026). Only the interactive Native TUI frontend sets
    /// this; it never registers or authorizes the model Bash Tool, and the
    /// SDK/one-shot/JSON/RPC paths never gain a User Shell.
    bool provide_user_shell{false};
    std::optional<bool> project_trust_override;
    bool disable_project_skills{false};
    bool disable_prompt_templates{false};
    std::vector<std::string> prompt_template_paths;
    bool workspace_explicit{false};
    std::size_t max_queued_messages{agent::kDefaultMaxQueuedMessages};
    std::size_t max_queued_bytes{agent::kDefaultMaxQueuedBytes};
    /// Explicit turn cap for the assembled session; std::nullopt (the default)
    /// imposes no cap (ADR 0015).
    std::optional<int> max_turns{std::nullopt};
    std::filesystem::path workspace;
    /// Normalized CLI session intent. Default construction selects
    /// workspace-keyed default persisted creation; explicit create and resume
    /// alternatives keep their exact paths; in-memory creation publishes no
    /// filesystem state.
    SessionTarget session_target{};
    /// Raw --session-dir value: the highest-priority CLI automatic-directory
    /// override, ahead of CCH_CODING_AGENT_SESSION_DIR and settings
    /// sessionDir. Consulted only for default persisted creation.
    std::optional<std::string> session_dir;
    /// pi CLI model selection: `--provider`, `--model`, `--models` patterns,
    /// and `--api-key` (in-memory runtime override). `--api-key` requires an
    /// explicit model at parse time.
    std::optional<std::string> provider;
    std::optional<std::string> model;
    std::vector<std::string> models;
    std::optional<std::string> api_key;
};

/// Internal result of SessionFactory::create(). The Sdk.cpp public
/// create_agent_session() wraps this into the public CreateAgentSessionResult.
struct CreateAgentSessionResult {
    /// The assembled runtime for the session.
    std::unique_ptr<AgentSessionRuntime> runtime;
    /// Diagnostics collected during creation (provider fallback,
    /// resource load warnings, etc.).
    std::vector<SdkDiagnostic> diagnostics;
    /// Resolved session metadata.
    std::string session_id;
    std::string provider;
    std::string model;
    std::optional<std::filesystem::path> session_path;
    std::filesystem::path workspace;
    harness::session::SessionMetadata metadata;
};

/// Assembles an AgentSessionRuntime (and its supporting services) from public
/// SDK creation options. Each creation attempt loads User Settings at most
/// once and assembles the session from that single snapshot; adapters never
/// supply a settings snapshot.
class SessionFactory {
public:
    [[nodiscard]] static util::Expected<CreateAgentSessionResult> create(
        CreateAgentSessionOptions options);
    /// Private integration-test/future Native TUI assembly path. Supplying a
    /// User Shell does not alter the public SDK options or model tool registry.
    [[nodiscard]] static util::Expected<CreateAgentSessionResult> create(
        CreateAgentSessionOptions options,
        std::unique_ptr<AsyncUserShell> user_shell);
    /// Private assembly seam for focused session tests. Models never appears
    /// in the installed SDK contract.
    [[nodiscard]] static util::Expected<CreateAgentSessionResult> create(
        CreateAgentSessionOptions options,
        std::shared_ptr<ai::Models> models);
    [[nodiscard]] static util::Expected<CreateAgentSessionResult> create(
        CreateAgentSessionOptions options,
        std::shared_ptr<ai::Models> models,
        std::unique_ptr<AsyncUserShell> user_shell);
    [[nodiscard]] static util::Expected<CreateAgentSessionResult> create(
        AgentSessionCreationRequest request);
};

} // namespace cch::coding_agent::runtime

namespace cch::coding_agent {

/// Private test-support wrapper around SessionFactory's Models assembly seam.
[[nodiscard]] util::Expected<CreateAgentSessionResult> create_agent_session_for_testing(
    CreateAgentSessionOptions options,
    std::shared_ptr<ai::Models> models);
[[nodiscard]] util::Expected<CreateAgentSessionResult> create_agent_session_for_testing(
    CreateAgentSessionOptions options,
    std::shared_ptr<ai::Models> models,
    std::unique_ptr<runtime::AsyncUserShell> user_shell);

} // namespace cch::coding_agent

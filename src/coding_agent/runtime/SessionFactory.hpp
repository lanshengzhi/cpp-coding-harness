#pragma once

#include <cch/coding_agent/Sdk.hpp>
#include <cch/coding_agent/Settings.hpp>
#include "coding_agent/runtime/AgentSessionRuntime.hpp"
#include "coding_agent/runtime/SessionLifecycle.hpp"

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
    std::optional<bool> project_trust_override;
    bool disable_project_skills{false};
    bool disable_prompt_templates{false};
    std::vector<std::string> prompt_template_paths;
    bool workspace_explicit{false};
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
    CliProviderOverrides provider_overrides;
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
    [[nodiscard]] static util::Expected<CreateAgentSessionResult> create(
        AgentSessionCreationRequest request);
};

} // namespace cch::coding_agent::runtime

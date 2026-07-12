#pragma once

#include "../../../include/cch/coding_agent/Sdk.hpp"
#include "../../../include/cch/coding_agent/Config.hpp"
#include "AgentSessionRuntime.hpp"
#include "SessionLifecycle.hpp"

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
    int max_turns{30};
    std::filesystem::path workspace;
    std::filesystem::path session_path;
    std::filesystem::path resume_path;
    CliProviderOverrides provider_overrides;
    ConfigData config;
    CommandRegistry command_registry;
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
    std::filesystem::path session_path;
    std::filesystem::path workspace;
    harness::session::SessionMetadata metadata;
};

/// Assembles an AgentSessionRuntime (and its supporting services) from public
/// SDK creation options.
class SessionFactory {
public:
    [[nodiscard]] static util::Expected<CreateAgentSessionResult> create(
        CreateAgentSessionOptions options);
    [[nodiscard]] static util::Expected<CreateAgentSessionResult> create(
        AgentSessionCreationRequest request);
};

} // namespace cch::coding_agent::runtime

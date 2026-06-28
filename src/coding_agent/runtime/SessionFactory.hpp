#pragma once

#include "../../../include/cch/coding_agent/Sdk.hpp"
#include "AgentSessionRuntime.hpp"
#include "SessionLifecycle.hpp"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace cch::coding_agent::runtime {

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
};

/// Assembles an AgentSessionRuntime (and its supporting services) from public
/// SDK creation options.
class SessionFactory {
public:
    [[nodiscard]] static util::Expected<CreateAgentSessionResult> create(
        CreateAgentSessionOptions options);
};

} // namespace cch::coding_agent::runtime

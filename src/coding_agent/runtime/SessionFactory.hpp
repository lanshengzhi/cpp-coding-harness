#pragma once

#include <cch/ai/Models.hpp>
#include <cch/coding_agent/Settings.hpp>
#include "coding_agent/AgentSession.hpp"
#include "coding_agent/ProjectResourceLoader.hpp"
#include "coding_agent/runtime/AgentSessionCreationRequest.hpp"
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

/// Private test-support assembly seam carried as one value: an injected
/// Models (the deterministic scripted provider catalog) and/or a Session-
/// owned User Shell (User Bash overlap/cancellation tests). Production
/// assembly passes nothing.
struct AssemblyOverrides {
    std::shared_ptr<ai::Models> models;
    std::unique_ptr<AsyncUserShell> user_shell;
};

/// Assembles an AgentSessionRuntime (and its supporting services) from the
/// internal CLI creation request and publishes it as the one publication
/// bundle (`coding_agent::CreateAgentSessionResult`). Each creation attempt
/// loads User Settings at most once and assembles the session from that
/// single snapshot; adapters never supply a settings snapshot.
class SessionFactory {
public:
    [[nodiscard]] static support::Expected<coding_agent::CreateAgentSessionResult>
    create(AgentSessionCreationRequest request, AssemblyOverrides overrides = {});

    /// Publication step used by the assembly implementation: bind the
    /// assembled runtime into the session handle and carry the bundle
    /// together. The class itself is the private implementation of the
    /// `create_agent_session` boundary, so this member is Owner-internal by
    /// construction.
    [[nodiscard]] static coding_agent::CreateAgentSessionResult publish(
        std::unique_ptr<AgentSessionRuntime> runtime,
        std::optional<std::filesystem::path> session_path,
        std::vector<coding_agent::SessionDiagnostic> diagnostics,
        std::optional<std::string> model_fallback_message,
        std::vector<coding_agent::LoadedThemeResource> theme_resources,
        coding_agent::ResolvedSessionIdentity identity);
};

} // namespace cch::coding_agent::runtime

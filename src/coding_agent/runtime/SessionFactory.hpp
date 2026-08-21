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
    /// The one Session Assembly door. A request accompanied by CLI-owned
    /// `session_facts` (the in-session replacement flows, pi `createRuntime`)
    /// has them re-applied under the issue #507 field-ownership rules before
    /// any assembly step reads the request; a host-built request (boot, print
    /// mode, list-models) arrives complete and passes no facts.
    [[nodiscard]] static support::Expected<coding_agent::CreateAgentSessionResult>
    create(AgentSessionCreationRequest request,
           std::optional<InteractiveSessionFacts> session_facts,
           AssemblyOverrides overrides = {});

    /// The CLI-facts merge the door performs (issue #507, absorbed from the
    /// interactive composition host): engine-resolved session trust wins —
    /// `project_trust_override` is filled from the facts only when the engine
    /// left it unset; the pure CLI-owned resource and model facts are
    /// re-applied unconditionally. The host-only capabilities (User Shell,
    /// Runtime target, shared Models runtime) are not part of the merge; the
    /// host sets them on the request.
    static void apply_cli_facts(
        AgentSessionCreationRequest& request,
        const InteractiveSessionFacts& facts);

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

#pragma once

#include <cch/ai/Models.hpp>
#include <cch/coding_agent/Settings.hpp>
#include <cch/support/AsyncResult.hpp>
#include "coding_agent/AgentSession.hpp"
#include "coding_agent/ProjectResourceLoader.hpp"
#include "coding_agent/runtime/AgentSessionAssembly.hpp"
#include "coding_agent/runtime/AgentSessionCreationRequest.hpp"
#include "coding_agent/runtime/AsyncUserShell.hpp"
#include "coding_agent/runtime/SessionLifecycle.hpp"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace cch::harness {
class RuntimeRoot;
}

namespace cch::coding_agent::runtime {

/// Private test-support assembly seam carried as one value: an injected
/// ModelRuntime built from scripted Provider Definitions and/or a Session-
/// owned User Shell (User Bash overlap/cancellation tests). The legacy Models
/// member remains only for tests that have not yet migrated to the runtime
/// seam. Production assembly passes neither override.
struct AssemblyOverrides {
    std::shared_ptr<ModelRuntime> model_runtime;
    bool cli_fake{false};
    std::shared_ptr<ai::Models> models;
    std::unique_ptr<AsyncUserShell> user_shell;
};

/// Assembles one Agent Session (and its supporting services) from the
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
    /// The asynchronous production Session Assembly door. The operation is
    /// lazy and consumes the request once; filesystem work uses the
    /// composition-authorized capability collection and cancellation is
    /// propagated through `stop_token`.
    [[nodiscard]] static support::AsyncResult<coding_agent::CreateAgentSessionResult> create_async(
            AgentSessionCreationRequest request,
            std::optional<InteractiveSessionFacts> session_facts,
            AssemblyOverrides overrides = {},
            std::stop_token stop_token = {});

    /// Temporary synchronous expand-contract bridge for tests and legacy
    /// callers. Production CLI paths use `create_async` and await it on their
    /// Runtime loop.
    [[nodiscard]] static support::Expected<coding_agent::CreateAgentSessionResult>
    create(AgentSessionCreationRequest request,
           std::optional<InteractiveSessionFacts> session_facts,
           AssemblyOverrides overrides = {});

    /// Compose all filesystem capabilities that Session Assembly may use.
    /// Every returned capability shares the request's one Runtime root target;
    /// explicit resource paths are authorized only when they fall under one
    /// of the known workspace, ancestor, Agent Config, or user roots.
    [[nodiscard]] static ProjectResourceFileSystems make_authorized_project_resource_filesystems(
            std::shared_ptr<harness::RuntimeRoot> runtime_root,
            std::filesystem::path workspace,
            std::filesystem::path agent_config_directory,
            std::filesystem::path home_directory,
            std::vector<std::string> explicit_paths = {});

    /// The CLI-facts merge the door performs (issue #507, absorbed from the
    /// interactive composition host): engine-resolved session trust wins —
    /// `project_trust_override` is filled from the facts only when the engine
    /// left it unset; the embedded `session_facts` value is replaced
    /// unconditionally. The host-only capabilities (User Shell, Runtime
    /// target, shared Models runtime) are not part of the merge; the host sets
    /// them on the request.
    static void apply_cli_facts(
        AgentSessionCreationRequest& request,
        const InteractiveSessionFacts& facts);

    /// Publication step used by the assembly implementation: hand the one
    /// assembled value to the session handle (which constructs its own Impl)
    /// and carry the bundle together. The class itself is the private
    /// implementation of the `create_agent_session` boundary, so this member
    /// is Owner-internal by construction.
    [[nodiscard]] static coding_agent::CreateAgentSessionResult publish(AgentSessionAssembly assembly,
            std::vector<coding_agent::SessionDiagnostic> diagnostics,
            std::optional<std::string> model_fallback_message,
            std::vector<coding_agent::LoadedThemeResource> theme_resources,
            coding_agent::ResolvedSessionIdentity identity);
};

} // namespace cch::coding_agent::runtime

#pragma once

#include "../../../include/cch/agent/ToolRegistry.hpp"
#include "../../../include/cch/coding_agent/ModelRuntime.hpp"
#include "../../../include/cch/coding_agent/Settings.hpp"
#include "../../../include/cch/harness/LocalExecutionEnv.hpp"
#include "coding_agent/runtime/AsyncUserShell.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace cch::coding_agent::runtime {

/// Passive bundle of runtime capabilities assembled by SessionFactory.
/// Kept as an implementation detail; SessionFactory is the only seam that
/// constructs or populates this bundle.
struct RuntimeServices {
    /// The session's canonical model/auth runtime, held as `std::shared_ptr`
    /// and injected into the stateful Agent (the sole injectable seam per
    /// #326). Always constructed by SessionFactory: a host-injected runtime
    /// wins, otherwise one is default-created from the Agent Config Directory.
    std::shared_ptr<ModelRuntime> model_runtime;
    /// True when the session created the runtime (default-created or the
    /// private test seam's wrap) and must release it on close. A host-injected
    /// runtime is never disposed by the session (ADR 0029).
    bool model_runtime_owned{false};
    /// The two-scope settings manager the session was assembled under (pi
    /// `AgentSession.settingsManager`): the runtime persists thinking-level
    /// (and later model) defaults to settings.json with the same project-trust
    /// state as creation. Empty when assembly had no settings surface.
    std::optional<coding_agent::SettingsManager> settings_manager;
    std::shared_ptr<harness::AsyncExecutionEnv> env;
    /// True when the factory created the execution environment and must clean
    /// it up on session close. Host-provided environments are never owned.
    bool env_owned{true};
    /// Independently owned direct-user capability; absence keeps User Bash
    /// unavailable without changing model tool authorization.
    std::unique_ptr<AsyncUserShell> user_shell;
    agent::AsyncToolRegistry tools;
};

} // namespace cch::coding_agent::runtime

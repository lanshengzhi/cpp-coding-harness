#pragma once

#include "../../../include/cch/agent/ToolRegistry.hpp"
#include "../../../include/cch/ai/ChatClient.hpp"
#include "../../../include/cch/coding_agent/ModelRuntime.hpp"
#include "../../../include/cch/harness/LocalExecutionEnv.hpp"
#include "coding_agent/runtime/AsyncUserShell.hpp"

#include <filesystem>
#include <memory>
#include <string>

namespace cch::coding_agent::runtime {

/// Passive bundle of runtime capabilities assembled by SessionFactory.
/// Kept as an implementation detail; SessionFactory is the only seam that
/// constructs or populates this bundle.
struct RuntimeServices {
    std::shared_ptr<ai::StreamingChatClient> stream;
    /// The session's canonical model/auth runtime. Nullable on legacy/CLI
    /// assembly paths that predate ModelRuntime injection; the Agent uses
    /// `stream` for requests.
    std::shared_ptr<ModelRuntime> model_runtime;
    /// True when the session created the runtime (default-created or the
    /// private test seam's wrap) and must release it on close. A host-injected
    /// runtime is never disposed by the session (ADR 0029).
    bool model_runtime_owned{false};
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

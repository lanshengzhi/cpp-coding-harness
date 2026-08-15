#pragma once

#include <cch/agent/AgentContext.hpp>
#include <cch/agent/AgentTool.hpp>
#include <cch/ai/Model.hpp>
#include <cch/coding_agent/ModelRuntime.hpp>
#include <cch/agent/harness/session/SessionTree.hpp>
#include <cch/agent/tools/ToolFactories.hpp>
#include "coding_agent/SessionTarget.hpp"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace cch::harness {
class RuntimeTarget;
}

namespace cch::coding_agent::runtime {

/// pi in-memory `createBranchedSession`: the derived branch facts seeded into
/// a newly created in-memory session. The C++ in-memory store is a no-op, so
/// the branch's message projection and derived state travel as a seed; the
/// in-session fork flow carries it through `in_memory_branch_seed`.
struct InMemoryBranchSeed {
    /// Branch context projection: messages, model, thinking level (pi
    /// `createBranchedSession` fileEntries minus the header, projected like
    /// `buildSessionContext`).
    harness::session::SessionContext context;
    /// The source session file (pi header `parentSession`); absent when the
    /// source session is itself in-memory.
    std::optional<std::filesystem::path> parent_session;
};

/// Internal creation request shared by the CLI adapters. Session assembly is
/// SessionFactory-authoritative; the request carries only CLI-owned facts
/// (workspace, session intent, model selection) plus the two private test
/// seams documented on their fields. The former public SDK creation options
/// (host skills/templates, tool selection, execution environment, agent dir,
/// trust store) are removed with the SDK (pi-coding-agent phase, ADR 0036).
struct AgentSessionCreationRequest {
    /// Assemble the Native TUI's independent Session-owned User Shell
    /// capability (ADR 0026). Only the interactive frontend sets this; it
    /// never registers or authorizes the model Bash Tool, and the one-shot
    /// print path never gains a User Shell.
    bool provide_user_shell{false};
    /// Target-bound admission and mailbox handle created by the one CLI Runtime
    /// root. Empty only in focused construction tests that do not execute
    /// filesystem or model Shell work.
    std::shared_ptr<harness::RuntimeTarget> execution_runtime_target;
    std::optional<bool> project_trust_override;
    /// pi `--no-skills`: drops user and project skill discovery (explicit
    /// `--skill` paths stay).
    bool no_skills{false};
    /// Repeatable pi `--skill` paths (files or directories): explicit skills
    /// load first (they win name collisions) and stay effective under
    /// `--no-skills`.
    std::vector<std::string> skill_paths;
    /// pi `--no-prompt-templates`: drops user and project prompt discovery
    /// (explicit `--prompt-template` paths still load).
    bool no_prompt_templates{false};
    std::vector<std::string> prompt_template_paths;
    /// pi `--no-themes`: drops user and project theme discovery (explicit
    /// `--theme` paths stay).
    bool no_themes{false};
    /// Repeatable pi `--theme` paths (files or directories): explicit theme
    /// inputs load after every discovered source and stay effective under
    /// `--no-themes`.
    std::vector<std::string> theme_paths;
    /// pi `--no-context-files`: disables Project Context File discovery
    /// (global AGENTS.md/CLAUDE.md + the cwd ancestor chain). Context files
    /// are never Project Trust gated (pinned fact).
    bool no_context_files{false};
    /// pi `--system-prompt`: raw text-or-file value; wins over SYSTEM.md
    /// discovery and renders as the custom-prompt branch of the System
    /// Prompt.
    std::optional<std::string> system_prompt;
    /// pi `--append-system-prompt` (repeatable): raw text-or-file values;
    /// win over APPEND_SYSTEM.md discovery and join with `"\n\n"` into the
    /// append section.
    std::vector<std::string> append_system_prompt;
    std::size_t max_queued_messages{agent::kDefaultMaxQueuedMessages};
    std::size_t max_queued_bytes{agent::kDefaultMaxQueuedBytes};
    /// The internal workspace containment seam: always the current working
    /// directory (pi `workspace := cwd`).
    std::filesystem::path workspace;
    /// Normalized CLI session intent. Default construction selects
    /// workspace-keyed default persisted creation; explicit create and resume
    /// alternatives keep their exact paths; in-memory creation publishes no
    /// filesystem state.
    SessionTarget session_target{};
    /// pi `--name`: the session display name, appended as a `session_info`
    /// entry after publication (pi appendSessionInfo sanitization). The CLI
    /// already enforced the non-empty guard.
    std::optional<std::string> session_name;
    /// pi `switchSession` cwdOverride (in-session resume only): bind the
    /// resumed session's runtime to this cwd even when the session header
    /// stores a different (missing) cwd. The header keeps its stored value
    /// (pi `SessionManager.open(path, dir, cwdOverride)`). The boot path
    /// never sets it.
    std::optional<std::filesystem::path> resume_cwd_override;
    /// Private in-session seam: the pi in-memory `createBranchedSession`
    /// seed for a newly created in-memory session. Meaningful only with
    /// `InMemorySessionTarget`; production callers never set it.
    std::optional<InMemoryBranchSeed> in_memory_branch_seed;
    /// Raw --session-dir value: the highest-priority CLI automatic-directory
    /// override, ahead of PI_CODING_AGENT_SESSION_DIR and settings
    /// sessionDir. Consulted only for default persisted creation.
    std::optional<std::string> session_dir;
    /// pi CLI model selection: `--provider`, `--model`, `--models` patterns,
    /// and `--api-key` (in-memory runtime override). `--api-key` requires an
    /// explicit model at parse time.
    std::optional<std::string> provider;
    std::optional<std::string> model;
    std::vector<std::string> models;
    std::optional<std::string> api_key;
    /// Private test seam: an explicit request Model for the injected-Models
    /// assembly path. Production callers never set it — the CLI resolves the
    /// request model through the pi chain (`--model`, resume re-resolution,
    /// settings default, runtime default). Focused session tests that need a
    /// deterministic request model (compaction thresholds, auth guidance)
    /// supply it here.
    std::optional<ai::Model> request_model;
    /// The host-shared ModelRuntime the Session uses (ADR 0029/0030, issue
    /// #466): the interactive CLI host builds one runtime and passes it to
    /// the boot Session and every in-session replacement, so Runtime loop,
    /// worker capacity, and model/auth resources are reused rather than
    /// reconstructed. The Session never owns or releases a host-injected
    /// runtime, so closing one Session keeps the shared Models resources the
    /// replacement Session needs. The private test seam injects a fake
    /// ModelRuntime through it; absent, the factory default-creates a
    /// Session-owned runtime from the Agent Config Directory.
    std::shared_ptr<coding_agent::ModelRuntime> model_runtime;
    /// Private test seam: custom tools registered alongside the fixed built-in
    /// tool set. Production callers never set it (the fixed #331 tool set is
    /// always available); retry-continuation tests inject recording tools.
    std::vector<agent::Tool> custom_tools;
    /// Private test seam: the shared live PI_* facts holder wired into the
    /// model Bash Tool. Production callers never set it (SessionFactory
    /// creates and wires the holder); focused tests capture it to assert the
    /// session refreshes the facts as the model and thinking level change.
    std::shared_ptr<tools::BashSessionEnvironment> bash_session_environment;
};

} // namespace cch::coding_agent::runtime

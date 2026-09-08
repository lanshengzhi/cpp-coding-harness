#pragma once

#include <cch/agent/AgentContext.hpp>
#include <cch/agent/harness/session/SessionEntry.hpp>
#include <cch/agent/harness/session/SessionResume.hpp>
#include <cch/support/Error.hpp>

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace cch::coding_agent {

/// Maximum bytes retained for one tool execution's current output tail in the
/// projection read model. The full output, when available, is represented by
/// an artifact reference rather than copied into every subscriber mailbox.
inline constexpr std::size_t kProjectionToolOutputTailBytes = 8 * 1024;

/// Maximum logical lines retained for one tool execution's current output tail.
inline constexpr std::size_t kProjectionToolOutputTailLines = 2000;

/// Current state of one tool execution in the projection read model.
///
/// This is deliberately a presentation value, not the Agent's execution
/// object. A subscriber can rebuild the currently visible tool block after a
/// late attach or mailbox overflow without replaying lifecycle events.
enum class ToolExecutionStatus {
    Running,
    Succeeded,
    Failed,
};

/// Recoverable presentation state for one tool call. Output is a bounded,
/// redacted cumulative tail; an artifact reference identifies the complete
/// output when the tool produced one. The call id is the correlation key.
struct ToolExecutionSnapshot {
    std::string tool_call_id{};
    std::string tool_name{};
    std::string arguments_json{};
    ToolExecutionStatus status{ToolExecutionStatus::Running};
    std::string output_tail{};
    bool output_truncated{false};
    std::optional<std::string> artifact_reference{std::nullopt};
    std::optional<std::string> error{std::nullopt};
};

/// Coarse phase of the current prompt/run. Terminal is retained after the
/// phase returns to Idle so observers can render the last outcome without
/// remembering an earlier event.
enum class RunPhase {
    Idle,
    Running,
    Retrying,
    Compacting,
};

enum class RunTerminalState {
    None,
    Succeeded,
    Failed,
    Cancelled,
};

/// Recoverable state for the current prompt/run.
struct RunState {
    RunPhase phase{RunPhase::Idle};
    RunTerminalState terminal{RunTerminalState::None};
    std::optional<std::string> error{std::nullopt};
};

enum class CompactionState {
    Idle,
    Running,
    Succeeded,
    Failed,
    Cancelled,
};

/// Recovery state needed by a new observer to render retry and compaction
/// progress without consuming the old session-event channel.
struct RecoveryState {
    std::size_t retry_count{0};
    std::size_t max_retry_count{0};
    std::optional<ai::TimestampMs> next_retry_at_ms{std::nullopt};
    std::optional<std::string> retry_error{std::nullopt};
    CompactionState compaction{CompactionState::Idle};
    std::optional<std::string> compaction_reason{std::nullopt};
    std::optional<std::string> compaction_error{std::nullopt};
};

/// Independent passive presentation state for one Agent Session.
///
/// `agent_state` is copied directly from the authoritative Agent. Session
/// metadata, the reconstructed active-path topology, the optional durable
/// path, and the run/recovery read model are copied from the assembled
/// session. Mutating any value in this snapshot cannot mutate the Agent,
/// Agent Session, or Session Store.
struct AgentSessionSnapshot {
    agent::AgentState agent_state{};
    harness::session::SessionMetadata metadata{};
    harness::session::SessionTopology topology{harness::session::SessionTopology::Linear};
    std::optional<std::filesystem::path> session_path{std::nullopt};
    /// Workspace and provider availability values used by the frontend footer.
    /// They are copied into the read model so a projection never reaches back
    /// into the live Session or ModelRuntime for business state.
    std::filesystem::path workspace{};
    bool using_subscription{false};
    std::size_t available_provider_count{0};
    std::vector<ToolExecutionSnapshot> tool_executions{};
    RunState run_state{};
    RecoveryState recovery_state{};
    /// Bounded, redacted diagnostics for session-event observer failures
    /// (ADR 0017), mirroring `agent_state.diagnostics` for the session
    /// assembly's own weak-observer registry.
    std::vector<support::Error> session_event_diagnostics{};
};

} // namespace cch::coding_agent

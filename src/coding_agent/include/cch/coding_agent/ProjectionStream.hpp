#pragma once

#include <cch/coding_agent/AgentSessionSnapshot.hpp>

#include <cch/ai/Message.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace cch::coding_agent {

class AgentSession;

/// Bounded retention per Projection subscriber (ADR 0052 Subscription
/// Mailbox). At the measured streaming publication rate (~50–100 bumps/s) 64
/// messages buffer well over half a second of Core activity between ticker
/// frames (~30/s), while keeping a slow subscriber's retention bounded; a
/// subscriber that still falls behind silently degrades to snapshot
/// consumption through a fresh Base instead of growing without bound.
inline constexpr std::size_t kProjectionMailboxCapacity = 64;

/// Base (ADR 0052): a complete immutable `AgentSessionSnapshot` captured at
/// attach or mailbox-overflow resync time — the subscriber's starting
/// picture, including the full conversation history. A mid-session attach
/// receives the state as it is now, never a replay from session start.
struct ProjectionStreamBase {
    /// Core publication version the snapshot was captured at.
    std::uint64_t version{0};
    AgentSessionSnapshot snapshot{};
};

/// Degenerate Patch (ADR 0052): one coarse mutation carrying the complete
/// snapshot value. Published for whole-value mutations (model/thinking
/// switches, reload, compaction rebuilds, tree navigation, run settle,
/// retry history pop, Close) where no finer slice applies.
struct SessionSnapshotPatch {
    AgentSessionSnapshot snapshot{};
};

/// One message appended to the live conversation history (the streaming
/// hot path: every committed user, assistant, and tool-result message).
struct MessageAppendedPatch {
    ai::MessageVariant message{};
};

/// New value of the in-flight streaming assistant partial (absent when no
/// partial is streaming).
struct StreamingMessagePatch {
    std::optional<ai::AssistantMessage> streaming_message{std::nullopt};
};

/// New value of the pending tool-call ids observed by the Agent.
struct PendingToolCallsPatch {
    std::vector<std::string> pending_tool_call_ids{};
};

/// The Agent run started (published with the `AgentStart` bump; the run's
/// settle republishes the whole state through a degenerate patch).
struct IsRunningPatch {
    bool is_running{false};
};

/// New value of both Agent-owned pending input queues. Published only when
/// a queue's message count changed since the last publication (queued
/// messages are append- and remove-only, so equal counts mean equal
/// content).
struct InputQueuesPatch {
    agent::AgentInputQueues input_queues{};
};

/// New value of the Agent's bounded observer-failure diagnostics channel.
/// Published only when the channel's serial changed since the last
/// publication (the vector rolls over, so its size cannot detect a new
/// entry).
struct AgentDiagnosticsPatch {
    std::vector<support::Error> diagnostics{};
};

/// Tool fact (ADR 0052): a tool execution started. The value-bearing
/// `execution` field is the recoverable read-model slice; it contains the
/// bounded arguments and output presentation needed by a late observer.
struct ToolStartedPatch {
    ToolExecutionSnapshot execution{};
};

/// Tool fact (ADR 0052): a cumulative partial tool result streamed while the
/// tool executes. The patch carries only the bounded read-model value, never
/// the unbounded cumulative result that used to make this push-only.
struct ToolPartialPatch {
    ToolExecutionSnapshot execution{};
};

/// Tool fact (ADR 0052): a tool execution reached its terminal outcome (the
/// result message itself is appended to the history through a
/// `MessageAppendedPatch`).
struct ToolFinishedPatch {
    ToolExecutionSnapshot execution{};
};

/// New value of the current prompt/run read-model state.
struct RunStatePatch {
    RunState run_state{};
};

/// New value of retry and compaction recovery state.
struct RecoveryStatePatch {
    RecoveryState recovery_state{};
};

/// Patch (ADR 0052): a value-bearing record of one published state change —
/// which slice changed and its new value. Tool and recovery facts are
/// snapshot-backed read-model slices, so a Base followed by patches is
/// sufficient to rebuild the state shown by a frontend.
using ProjectionStreamPatchVariant = std::variant<SessionSnapshotPatch,
        MessageAppendedPatch,
        StreamingMessagePatch,
        PendingToolCallsPatch,
        IsRunningPatch,
        InputQueuesPatch,
        AgentDiagnosticsPatch,
        ToolStartedPatch,
        ToolPartialPatch,
        ToolFinishedPatch,
        RunStatePatch,
        RecoveryStatePatch>;

/// An ordered batch of value-bearing Patches published at one version.
/// Applying every batch in order to a Base reproduces the later snapshot
/// exactly (ADR 0052 convergence invariant).
struct ProjectionStreamPatchMsg {
    /// Core publication version this batch belongs to.
    std::uint64_t version{0};
    std::vector<ProjectionStreamPatchVariant> patches{};
};

/// The exactly two message kinds a Projection listener receives (ADR 0052).
using ProjectionStreamMessageVariant = std::variant<ProjectionStreamBase, ProjectionStreamPatchMsg>;

/// Projection listener (ADR 0052): invoked by `ProjectionSubscription::drain`
/// for every queued stream message, on the draining domain. The listener must
/// perform only bounded value work, must never throw, and must not call back
/// into the Headless Core; a throwing listener is a fatal contract violation
/// (strict no-exceptions Runtime, CODING_STANDARDS.md §5.7), not a recoverable
/// failure — publication never blocks on a listener (ADR 0052).
using ProjectionStreamSink = std::move_only_function<void(const ProjectionStreamMessageVariant&)>;

/// Apply one ordered patch batch onto a Base-composed snapshot (ADR 0052
/// convergence application). Every projection-relevant fact, including tool
/// progress and recovery state, updates a read-model slice; no projection
/// needs to remember a push-only event.
inline void apply_projection_patches(
        AgentSessionSnapshot& composed, const std::vector<ProjectionStreamPatchVariant>& patches) {
    const auto apply_tool = [&composed](ToolExecutionSnapshot execution) {
        if (execution.tool_call_id.empty()) return;
        const auto found = std::find_if(composed.tool_executions.begin(),
                composed.tool_executions.end(),
                [&execution](const ToolExecutionSnapshot& current) {
                    return current.tool_call_id == execution.tool_call_id;
                });
        if (found == composed.tool_executions.end()) {
            composed.tool_executions.push_back(std::move(execution));
        } else {
            *found = std::move(execution);
        }
    };

    for (const auto& patch : patches) {
        if (const auto* value = std::get_if<SessionSnapshotPatch>(&patch)) {
            composed = value->snapshot;
        } else if (const auto* value = std::get_if<MessageAppendedPatch>(&patch)) {
            composed.agent_state.messages.push_back(value->message);
        } else if (const auto* value = std::get_if<StreamingMessagePatch>(&patch)) {
            composed.agent_state.streaming_message = value->streaming_message;
        } else if (const auto* value = std::get_if<PendingToolCallsPatch>(&patch)) {
            composed.agent_state.pending_tool_call_ids = value->pending_tool_call_ids;
        } else if (const auto* value = std::get_if<IsRunningPatch>(&patch)) {
            composed.agent_state.is_running = value->is_running;
        } else if (const auto* value = std::get_if<InputQueuesPatch>(&patch)) {
            composed.agent_state.input_queues = value->input_queues;
        } else if (const auto* value = std::get_if<AgentDiagnosticsPatch>(&patch)) {
            composed.agent_state.diagnostics = value->diagnostics;
        } else if (const auto* value = std::get_if<ToolStartedPatch>(&patch)) {
            apply_tool(value->execution);
        } else if (const auto* value = std::get_if<ToolPartialPatch>(&patch)) {
            apply_tool(value->execution);
        } else if (const auto* value = std::get_if<ToolFinishedPatch>(&patch)) {
            apply_tool(value->execution);
        } else if (const auto* value = std::get_if<RunStatePatch>(&patch)) {
            composed.run_state = value->run_state;
        } else if (const auto* value = std::get_if<RecoveryStatePatch>(&patch)) {
            composed.recovery_state = value->recovery_state;
        }
    }
}

/// RAII handle for one Projection Stream subscriber (ADR 0052). Destroying or
/// calling unsubscribe() stops delivery and deactivates the subscriber's
/// mailbox; the Headless Core prunes it at its next publication. A closed
/// session retains a live subscription: queued messages still drain, no
/// further publication occurs.
class ProjectionSubscription {
public:
    ProjectionSubscription() = default;
    ProjectionSubscription(ProjectionSubscription&&) noexcept;
    ProjectionSubscription& operator=(ProjectionSubscription&&) noexcept;
    ~ProjectionSubscription();
    ProjectionSubscription(const ProjectionSubscription&) = delete;
    ProjectionSubscription& operator=(const ProjectionSubscription&) = delete;

    /// Unsubscribe from further publication. Idempotent.
    void unsubscribe();

    /// Deliver every queued stream message to the subscriber's listener in
    /// publication order. Overflow already resynchronized the mailbox with a
    /// fresh Base (ADR 0052); drain never errors and never blocks. Returns
    /// the number of messages delivered.
    [[nodiscard]] std::size_t drain();

    /// True while the subscription remains registered.
    [[nodiscard]] explicit operator bool() const;

public:
    /// Opaque implementation type. Defined in the session implementation.
    struct Impl;

private:
    friend class AgentSession;
    explicit ProjectionSubscription(std::shared_ptr<Impl> state);
    std::shared_ptr<Impl> impl_;
};

} // namespace cch::coding_agent

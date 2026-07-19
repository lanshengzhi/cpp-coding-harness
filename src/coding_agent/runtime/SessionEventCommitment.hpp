#pragma once

#include "../../../include/cch/agent/AgentContext.hpp"
#include "../../../include/cch/agent/AgentEvent.hpp"
#include "../../../include/cch/ai/Message.hpp"
#include "../../../include/cch/harness/session/SessionStore.hpp"
#include "../../../include/cch/util/Error.hpp"

#include <deque>
#include <optional>
#include <vector>

namespace cch::coding_agent::runtime {

/// One subscriber registration in an Agent Session. Session Event Commitment
/// snapshots the active sinks when a run starts; subscribe/unsubscribe
/// changes during a run take effect on the next run.
struct SubscriberEntry {
    int id{-1};
    agent::AgentEventSink sink;
    bool active{true};
};

/// Session Event Commitment — the ordered policy that commits one agent
/// lifecycle event to an Agent Session:
///   1. advance Live Session State (state-first, matching pi's ordering);
///   2. deliver to the subscriber snapshot taken at run start;
///   3. append the Session Entry for incrementally persisted messages.
///
/// One instance exists per run and borrows the runtime-owned seams. The first
/// commitment failure aborts the run through the sink and is later reported
/// by conclude() unwrapped, ahead of any agent-loop error.
class SessionEventCommitment final {
public:
    SessionEventCommitment(
        std::vector<ai::MessageVariant>& live_history,
        std::deque<SubscriberEntry>& subscribers,
        harness::session::SessionStore& store);

    SessionEventCommitment(const SessionEventCommitment&) = delete;
    SessionEventCommitment& operator=(const SessionEventCommitment&) = delete;

    /// The sink handed to the agent loop for this run. The returned sink
    /// borrows this commitment and must not outlive it.
    [[nodiscard]] agent::AgentEventSink sink();

    /// The run's single final verdict: a recorded commitment failure first
    /// (unwrapped), then an unfinished loop, then the agent-loop result.
    [[nodiscard]] util::ExpectedVoid conclude(
        std::optional<util::Expected<agent::AsyncAgentRunResult>> loop_result) const;

private:
    std::vector<ai::MessageVariant>& live_history_;
    harness::session::SessionStore& store_;
    std::vector<agent::AgentEventSink*> snapshot_;
    std::optional<util::Error> failure_;
};

} // namespace cch::coding_agent::runtime

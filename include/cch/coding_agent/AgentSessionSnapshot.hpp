#pragma once

#include <cch/agent/AgentContext.hpp>
#include <cch/agent/harness/session/SessionEntry.hpp>
#include <cch/agent/harness/session/SessionResume.hpp>

#include <filesystem>
#include <optional>
#include <vector>

namespace cch::coding_agent {

/// Independent passive presentation state for one Agent Session.
///
/// `agent_state` is copied directly from the authoritative Agent. Session
/// metadata, the reconstructed active-path topology, and the optional durable
/// path are copied from the assembled session. Mutating any value in this
/// snapshot cannot mutate the Agent, Agent Session, or Session Store.
struct AgentSessionSnapshot {
    agent::AgentState agent_state{};
    harness::session::SessionMetadata metadata{};
    harness::session::SessionTopology topology{harness::session::SessionTopology::Linear};
    std::optional<std::filesystem::path> session_path{std::nullopt};
    /// Bounded, redacted diagnostics for session-event observer failures
    /// (ADR 0017), mirroring `agent_state.diagnostics` for the session
    /// assembly's own weak-observer registry.
    std::vector<util::Error> session_event_diagnostics{};
};

} // namespace cch::coding_agent

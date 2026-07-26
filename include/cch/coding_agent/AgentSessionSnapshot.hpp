#pragma once

#include <cch/agent/AgentContext.hpp>
#include <cch/harness/session/SessionEntry.hpp>
#include <cch/harness/session/SessionResume.hpp>

#include <filesystem>
#include <optional>

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
};

} // namespace cch::coding_agent

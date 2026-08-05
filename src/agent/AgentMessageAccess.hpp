#pragma once

#include <cch/agent/Agent.hpp>
#include <cch/ai/Message.hpp>
#include <cch/util/Error.hpp>

namespace cch::agent::detail {

/// Private runtime adapter for adding one completed passive message to
/// Agent-owned Live Session State without synthesizing lifecycle events.
class AgentMessageAccess {
public:
    [[nodiscard]] static util::ExpectedVoid append_bash_execution(
        Agent& agent,
        ai::BashExecutionMessage message);

    /// Replace the Agent's entire Live Session State message list with a
    /// rebuilt context (pi `agent.state.messages = sessionContext.messages`
    /// after compaction). Requires an idle Agent: an active run is rejected.
    [[nodiscard]] static util::ExpectedVoid replace_messages(
        Agent& agent,
        std::vector<ai::MessageVariant> messages);
};

} // namespace cch::agent::detail

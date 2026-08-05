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

    /// Drop the final Live Session State message when it is an assistant
    /// message (pi `this.agent.state.messages = messages.slice(0, -1)` before
    /// and after the overflow compact-and-retry). The message stays in session
    /// history; only the live context drops it so the retry does not re-send
    /// it. No-op when the last message is not an assistant message. Requires
    /// an idle Agent.
    [[nodiscard]] static util::ExpectedVoid pop_trailing_assistant(
        Agent& agent);
};

} // namespace cch::agent::detail

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
};

} // namespace cch::agent::detail

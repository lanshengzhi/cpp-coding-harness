#pragma once

#include <cch/agent/Agent.hpp>

#include <cch/support/AsyncResult.hpp>

#include <stop_token>
#include <string>
#include <utility>

namespace cch::agent::detail {

/// Private adapter for supplying the Agent Session's prompt-scoped stop source
/// without widening the public Agent execution contract.
class AgentPromptAccess {
public:
    [[nodiscard]] static support::AsyncResult<void> prompt(
        Agent& agent,
        ai::UserMessage user_message,
        AgentEventCommitter commitment,
        std::stop_source stop_source) {
        return agent.prompt(
            std::move(user_message),
            std::move(commitment),
            std::move(stop_source));
    }

    /// Continue the loop without a new user message (pi `agent.continue()` /
    /// `runAgentLoopContinue`), used by the session assembly's overflow
    /// compact-and-retry-once after compaction rebuilt the live context.
    [[nodiscard]] static support::AsyncResult<void> continue_run(
        Agent& agent,
        AgentEventCommitter commitment,
        std::stop_source stop_source) {
        return agent.continue_run(
            std::move(commitment),
            std::move(stop_source));
    }
};

} // namespace cch::agent::detail

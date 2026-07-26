#pragma once

#include <cch/agent/Agent.hpp>

#include <boost/asio/awaitable.hpp>

#include <stop_token>
#include <string>
#include <utility>

namespace cch::agent::detail {

/// Private adapter for supplying the Agent Session's prompt-scoped stop source
/// without widening the public Agent execution contract.
class AgentPromptAccess {
public:
    [[nodiscard]] static boost::asio::awaitable<util::ExpectedVoid> prompt(
        Agent& agent,
        ai::UserMessage user_message,
        AgentEventCommitter commitment,
        std::stop_source stop_source) {
        return agent.prompt(
            std::move(user_message),
            std::move(commitment),
            std::move(stop_source));
    }
};

} // namespace cch::agent::detail

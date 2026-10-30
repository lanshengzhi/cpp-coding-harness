#pragma once

#include <cch/agent/RunToolCall.hpp>

#include <boost/asio/awaitable.hpp>

namespace cch::agent {

[[nodiscard]] boost::asio::awaitable<AgentToolCallOutcome> run_tool_call_coro(
        ai::ToolCallContent tool_call, RunToolCallOptions options);

} // namespace cch::agent

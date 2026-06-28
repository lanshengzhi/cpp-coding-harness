#pragma once

#include "../../include/cch/agent/AgentContext.hpp"
#include "../../include/cch/agent/AgentEvent.hpp"
#include "../../include/cch/agent/AgentTool.hpp"
#include "../../include/cch/agent/ToolRegistry.hpp"
#include "../../include/cch/ai/Message.hpp"
#include "../../include/cch/ai/Tool.hpp"
#include "../../include/cch/util/Error.hpp"

#include <boost/asio/awaitable.hpp>

#include <cstddef>
#include <optional>
#include <vector>

namespace cch::agent {

struct ToolCallExecutorOptions {
    std::optional<BeforeToolCallHook> before_tool_call;
    std::optional<AfterToolCallHook> after_tool_call;
    ai::ToolExecutionMode mode{ai::ToolExecutionMode::Sequential};
    std::size_t max_parallel_tools{8};
};

struct ToolCallBatchResult {
    std::vector<ai::ToolResultMessage> results;
    bool terminate_batch{false};
};

class ToolCallExecutor {
public:
    ToolCallExecutor(const AsyncToolRegistry& registry, ToolCallExecutorOptions options);

    [[nodiscard]] boost::asio::awaitable<util::Expected<ToolCallBatchResult>> execute(
        int turn,
        const ai::AssistantMessage& assistant_message,
        const std::vector<ai::ToolCallContent>& calls,
        ai::AiContext& context,
        AgentState& state,
        AgentEventSink& sink);

private:
    [[nodiscard]] boost::asio::awaitable<util::Expected<ToolCallBatchResult>> execute_sequential(
        int turn,
        const ai::AssistantMessage& assistant_message,
        const std::vector<ai::ToolCallContent>& calls,
        ai::AiContext& context,
        AgentState& state,
        AgentEventSink& sink);

    [[nodiscard]] boost::asio::awaitable<util::Expected<ToolCallBatchResult>> execute_parallel(
        int turn,
        const ai::AssistantMessage& assistant_message,
        const std::vector<ai::ToolCallContent>& calls,
        ai::AiContext& context,
        AgentState& state,
        AgentEventSink& sink);

    const AsyncToolRegistry& registry_;
    ToolCallExecutorOptions options_;
};

} // namespace cch::agent

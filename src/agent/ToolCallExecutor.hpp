#pragma once

#include "../../include/cch/agent/AgentContext.hpp"
#include "../../include/cch/agent/AgentEvent.hpp"
#include "../../include/cch/agent/AgentTool.hpp"
#include "../../include/cch/agent/ToolRegistry.hpp"
#include "../../include/cch/ai/Context.hpp"
#include "../../include/cch/ai/Message.hpp"
#include "../../include/cch/util/Error.hpp"

#include <boost/asio/awaitable.hpp>

#include <optional>
#include <vector>

namespace cch::agent {

struct ToolCallExecutorOptions {
    std::optional<BeforeToolCallHook> before_tool_call;
    std::optional<AfterToolCallHook> after_tool_call;
    ToolExecutionPolicy execution{SequentialToolExecution{}};
};

struct ToolCallBatchRequest {
    const ai::AssistantMessage& assistant_message;
    const ai::AiContext& context;
};

struct ToolCallBatchResult {
    std::vector<ai::ToolResultMessage> results;
    bool terminate_batch{false};
};

class ToolCallExecutor {
public:
    ToolCallExecutor(const AsyncToolRegistry& registry, ToolCallExecutorOptions options);

    [[nodiscard]] boost::asio::awaitable<util::Expected<ToolCallBatchResult>> execute(
        ToolCallBatchRequest request,
        AgentEventSink& sink);

private:
    [[nodiscard]] boost::asio::awaitable<util::Expected<ToolCallBatchResult>> execute_sequential(
        ToolCallBatchRequest request,
        const std::vector<ai::ToolCallContent>& calls,
        AgentEventSink& sink);

    [[nodiscard]] boost::asio::awaitable<util::Expected<ToolCallBatchResult>> execute_parallel(
        ToolCallBatchRequest request,
        const std::vector<ai::ToolCallContent>& calls,
        std::size_t max_in_flight,
        AgentEventSink& sink);

    const AsyncToolRegistry& registry_;
    ToolCallExecutorOptions options_;
};

} // namespace cch::agent

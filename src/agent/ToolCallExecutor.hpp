#pragma once

#include "../../include/cch/agent/AgentContext.hpp"
#include "../../include/cch/agent/AgentEvent.hpp"
#include "../../include/cch/agent/AgentTool.hpp"
#include "../../include/cch/agent/ToolRegistry.hpp"
#include "../../include/cch/ai/Context.hpp"
#include "../../include/cch/ai/Message.hpp"
#include "../../include/cch/util/Error.hpp"

#include <boost/asio/awaitable.hpp>

#include <stop_token>
#include <vector>

namespace cch::agent {

struct ToolCallExecutorOptions {
    // Non-owning hooks borrowed from the run's owning AsyncAgentOptions; they
    // must outlive the ToolCallExecutor built from these options.
    BeforeToolCallHook* before_tool_call{nullptr};
    AfterToolCallHook* after_tool_call{nullptr};
    std::stop_token stop_token;
    ToolExecutionPolicy execution{BoundedParallelToolExecution{}};
};

struct ToolCallBatchRequest {
    const ai::AssistantMessage& assistant_message; // must outlive the execute coroutine
    const ai::AiContext& context;                  // must outlive the execute coroutine
};

struct ToolCallBatchResult {
    std::vector<ai::ToolResultMessage> results;
    bool terminate_batch{false};
};

class ToolCallExecutor {
public:
    ToolCallExecutor(const AsyncToolRegistry& registry, ToolCallExecutorOptions options);
    ToolCallExecutor(ToolCallExecutor&&) noexcept = default;
    ToolCallExecutor& operator=(ToolCallExecutor&&) = delete;
    ~ToolCallExecutor() = default;
    ToolCallExecutor(const ToolCallExecutor&) = delete;
    ToolCallExecutor& operator=(const ToolCallExecutor&) = delete;

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

    const AsyncToolRegistry& registry_; // must outlive every execute coroutine
    ToolCallExecutorOptions options_;
};

} // namespace cch::agent

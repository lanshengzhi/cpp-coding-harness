#pragma once

#include <cch/agent/AgentContext.hpp>
#include <cch/agent/AgentEvent.hpp>
#include <cch/agent/AgentTool.hpp>
#include <cch/agent/ToolRegistry.hpp>
#include <cch/ai/Context.hpp>
#include <cch/ai/Message.hpp>
#include <cch/support/Error.hpp>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/experimental/concurrent_channel.hpp>
#include <boost/system/error_code.hpp>

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

struct FinalizedToolCallResult {
    ai::ToolResultMessage result;
    bool call_terminate{false};
};

using ExecutionPermitChannel = boost::asio::experimental::concurrent_channel<
    void(boost::system::error_code)>;

struct ToolExecutionPermits {
    ExecutionPermitChannel* preparation{nullptr}; // non-owning; must outlive coroutine
    ExecutionPermitChannel* concurrency{nullptr}; // non-owning; must outlive coroutine
    ExecutionPermitChannel* after_hook{nullptr};  // non-owning; must outlive coroutine
};

class ToolCallExecutor {
public:
    ToolCallExecutor(ToolRegistry& registry, ToolCallExecutorOptions options);
    ToolCallExecutor(ToolCallExecutor&&) noexcept = default;
    ToolCallExecutor& operator=(ToolCallExecutor&&) = delete;
    ~ToolCallExecutor() = default;
    ToolCallExecutor(const ToolCallExecutor&) = delete;
    ToolCallExecutor& operator=(const ToolCallExecutor&) = delete;

    [[nodiscard]] boost::asio::awaitable<support::Expected<ToolCallBatchResult>> execute(
        ToolCallBatchRequest request, // referenced values must outlive the execute coroutine
        AgentEventSink& sink);

private:
    [[nodiscard]] boost::asio::awaitable<support::Expected<FinalizedToolCallResult>> execute_single_call(
        ToolCallBatchRequest request,       // referenced values must outlive the coroutine
        const ai::ToolCallContent& call,    // must outlive the coroutine
        AgentEventSink& sink,               // must outlive the coroutine
        ToolExecutionPermits permits = {}); // non-owning channels must outlive the coroutine

    [[nodiscard]] boost::asio::awaitable<void> execute_parallel_task(
        ToolCallBatchRequest request,       // referenced values must outlive the coroutine
        const ai::ToolCallContent& call,    // must outlive the coroutine
        std::size_t index,
        std::shared_ptr<struct ParallelExecutionState> state,
        AgentEventSink parallel_sink,
        ToolExecutionPermits permits);      // non-owning channels must outlive the coroutine

    [[nodiscard]] boost::asio::awaitable<support::Expected<ToolCallBatchResult>> execute_sequential(
        ToolCallBatchRequest request,
        const std::vector<ai::ToolCallContent>& calls,
        AgentEventSink& sink);

    [[nodiscard]] boost::asio::awaitable<support::Expected<ToolCallBatchResult>> execute_parallel(
        ToolCallBatchRequest request,
        const std::vector<ai::ToolCallContent>& calls,
        std::size_t max_in_flight,
        AgentEventSink& sink);

    ToolRegistry& registry_; // must outlive every execute coroutine
    ToolCallExecutorOptions options_;
};

} // namespace cch::agent

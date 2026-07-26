#pragma once

#include <cch/agent/AgentContext.hpp>
#include <cch/agent/AgentEvent.hpp>
#include <cch/agent/ToolRegistry.hpp>
#include <cch/ai/ChatClient.hpp>

#include <boost/asio/awaitable.hpp>

#include <stop_token>
#include <string>
#include <vector>

namespace cch::agent {

/// Private result carrier for one coroutine-loop invocation.
struct AsyncAgentRunResult {
    ai::AiContext context;
    ai::AssistantStopReason stop_reason{ai::AssistantStopReason::Stop};
    int turns{0};
    AgentState state;
};

/// Private execution machinery owned by the stateful Agent.
class AsyncAgentLoop {
public:
    AsyncAgentLoop(ai::StreamingChatClient& client, AsyncToolRegistry registry, AsyncAgentOptions options = {});

    [[nodiscard]] boost::asio::awaitable<util::Expected<AsyncAgentRunResult>> run(
        std::string user_prompt,
        AgentEventSink sink = {},
        std::stop_token stop_token = {});

    [[nodiscard]] boost::asio::awaitable<util::Expected<AsyncAgentRunResult>> continue_with(
        std::vector<ai::MessageVariant> history,
        std::string user_prompt,
        AgentEventSink sink = {},
        std::stop_token stop_token = {});

    [[nodiscard]] boost::asio::awaitable<util::Expected<AsyncAgentRunResult>> continue_with(
        std::vector<ai::MessageVariant> history,
        ai::UserMessage user_message,
        AgentEventSink sink = {},
        std::stop_token stop_token = {});

private:
    friend class Agent;

    [[nodiscard]] const ai::Model& current_model() const noexcept {
        return options_.model;
    }
    [[nodiscard]] const std::string& current_thinking_level() const noexcept {
        return options_.thinking_level;
    }

    ai::StreamingChatClient& client_; // must outlive every run coroutine
    AsyncToolRegistry registry_;
    AsyncAgentOptions options_;
};

} // namespace cch::agent

#pragma once

#include <cch/agent/AgentContext.hpp>
#include <cch/agent/AgentEvent.hpp>
#include <cch/agent/ToolRegistry.hpp>
#include <cch/ai/ChatClient.hpp>

#include <boost/asio/awaitable.hpp>

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
        AgentEventSink sink = {});

    [[nodiscard]] boost::asio::awaitable<util::Expected<AsyncAgentRunResult>> continue_with(
        std::vector<ai::MessageVariant> history,
        std::string user_prompt,
        AgentEventSink sink = {});

private:
    friend class Agent;

    [[nodiscard]] const std::string& current_model() const noexcept {
        return options_.model;
    }
    [[nodiscard]] const std::string& current_thinking_level() const noexcept {
        return options_.thinking_level;
    }

    [[nodiscard]] util::ExpectedVoid append_message_with_lifecycle(
        AgentState& state,
        ai::AiContext& context,
        AgentEventSink& sink,
        ai::MessageVariant message) const;
    [[nodiscard]] util::ExpectedVoid emit(AgentEventSink& sink, const AgentLifecycleEvent& event) const;
    [[nodiscard]] std::vector<ai::ToolCallContent> tool_calls(const ai::AssistantMessage& message) const;

    ai::StreamingChatClient& client_;
    AsyncToolRegistry registry_;
    AsyncAgentOptions options_;
};

} // namespace cch::agent
